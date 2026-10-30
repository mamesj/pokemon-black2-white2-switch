// RetroAchievements: see achievements.h.
#include "achievements.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "NDS.h"
#include "thirdparty/rcheevos/include/rc_client.h"
#include "thirdparty/rcheevos/include/rc_consoles.h"
#include "thirdparty/rcheevos/include/rc_hash.h"
#include "thirdparty/rcheevos/include/rc_api_user.h"

namespace Achievements
{
// The internet request, made by each platform its own way (achievements_http.cpp): a POST when
// `post` is not empty, otherwise a GET. False when it could not be made at all (no connection).
bool HttpPost(const std::string& url, const std::string& post, const std::string& contentType,
              const std::string& userAgent, int& status, std::string& body, std::string& error);
// The system's name for the user agent ("Windows", "Linux", ...).
const char* SystemName();

namespace
{
rc_client_t* g_client = nullptr;
Account* g_account = nullptr;
std::function<void()> g_save;
std::string g_userAgent;
std::atomic<melonDS::NDS*> g_nds{nullptr};
std::string g_hash;              // the game's fingerprint (empty: none, or not a game RetroAchievements knows)
std::atomic<bool> g_loaded{false};  // the game's set is loaded (and followed when not paused)
std::atomic<bool> g_cheats{false};
std::atomic<bool> g_loggingIn{false}, g_loading{false};
std::string g_gameLine;          // what GameLine says (main thread)
std::string g_loginError;

std::mutex g_msgMutex;
std::deque<std::string> g_messages;

// In the menus' font: capitals, digits and : - + . / < > ! ( ), others as spaces.
std::string Shown(const char* s, size_t max = 60)
{
    std::string o;
    bool space = false;
    for (const char* p = s ? s : ""; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(":-+./<>!()", c);
        if (!ok || c == ' ')
        {
            if (!o.empty() && !space) o += ' ';
            space = true;
            continue;
        }
        o += (char)c;
        space = false;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.size() > max) o = o.substr(0, max - 3) + "...";
    return o;
}

void Say(const std::string& m)
{
    fprintf(stderr, "[achievements] %s\n", m.c_str());
    std::lock_guard<std::mutex> lock(g_msgMutex);
    g_messages.push_back(m);
}

// ---------------------------------------------------------------- the server's requests
// rc_client asks for them from any thread (the emulation's, during a frame, for an unlock); one
// thread of their own makes them, and their answers go back to rc_client from the main thread
// (Pump), so a slow connection never holds the game up.
struct Request
{
    std::string url, post, contentType;
    rc_client_server_callback_t callback;
    void* data;
};
struct Response
{
    int status;
    std::string body;
    rc_client_server_callback_t callback;
    void* data;
};
std::mutex g_queueMutex;
std::condition_variable g_queueWake;
std::deque<Request> g_requests;
std::deque<Response> g_responses;
std::thread g_worker;
bool g_quit = false;

void Worker()
{
    for (;;)
    {
        Request r;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueWake.wait(lock, [] { return g_quit || !g_requests.empty(); });
            if (g_quit) return;
            r = std::move(g_requests.front());
            g_requests.pop_front();
        }
        Response a{0, {}, r.callback, r.data};
        std::string error;
        if (!HttpPost(r.url, r.post, r.contentType, g_userAgent, a.status, a.body, error))
        {
            // (A connection that failed: rc_client tries an unlock again later.)
            a.status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            a.body = error;
            fprintf(stderr, "[achievements] request failed: %s\n", error.c_str());
        }
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_responses.push_back(std::move(a));
    }
}

void RC_CCONV ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data, rc_client_t*)
{
    Request r{request->url ? request->url : "", request->post_data ? request->post_data : "",
              request->content_type ? request->content_type : "application/x-www-form-urlencoded", callback, data};
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_requests.push_back(std::move(r));
    g_queueWake.notify_one();
}

// ---------------------------------------------------------------- the DS's memory
// RetroAchievements' map of a DS: its 4 MB of main RAM from 0, and the ARM9's data TCM (where the
// games keep their stack and some of their state) at 0x1000000.
uint32_t RC_CCONV ReadMemory(uint32_t address, uint8_t* buffer, uint32_t bytes, rc_client_t*)
{
    melonDS::NDS* nds = g_nds.load();
    if (!nds) return 0;
    uint32_t done = 0;
    for (; done < bytes; done++)
    {
        const uint32_t a = address + done;
        if (a < 0x400000) buffer[done] = nds->MainRAM[a & nds->MainRAMMask];
        else if (a >= 0x1000000 && a < 0x1004000) buffer[done] = nds->ARM9.DTCM[a - 0x1000000];
        else break;
    }
    return done;
}

// ---------------------------------------------------------------- what happens
void RC_CCONV Event(const rc_client_event_t* e, rc_client_t*)
{
    switch (e->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (e->achievement)
            Say("ACHIEVEMENT: " + Shown(e->achievement->title, 40) + " (" + std::to_string(e->achievement->points) + ")");
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        Say("EVERY ACHIEVEMENT UNLOCKED!");
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        if (e->server_error) Say("RETROACHIEVEMENTS: " + Shown(e->server_error->error_message, 50));
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        Say("RETROACHIEVEMENTS: OFFLINE. UNLOCKS ARE SENT WHEN BACK ONLINE");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        Say("RETROACHIEVEMENTS: BACK ONLINE");
        break;
    default:
        break;
    }
}

void RC_CCONV Log(const char* message, const rc_client_t*)
{
    fprintf(stderr, "[rcheevos] %s\n", message);
}

// (Short: beside THIS GAME in the pause menu's box. What went wrong, if anything, is g_detail.)
std::string g_detail;
void UpdateGameLine()
{
    if (!g_account || !g_account->on) { g_gameLine = "-"; return; }
    if (g_loading) { g_gameLine = "LOADING..."; return; }
    if (!g_loaded)
    {
        if (g_gameLine.empty()) g_gameLine = "-";
        return;
    }
    if (g_cheats) { g_gameLine = "PAUSED (CHEATS ON)"; return; }
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(g_client, &s);
    g_gameLine = s.num_core_achievements == 0 ? "NONE YET"
               : std::to_string(s.num_unlocked_achievements) + " OF " + std::to_string(s.num_core_achievements) + " UNLOCKED";
}

void RC_CCONV GameLoaded(int result, const char* error, rc_client_t*, void*)
{
    g_loading = false;
    if (result == RC_OK)
    {
        g_loaded = true;
        const rc_client_game_t* game = rc_client_get_game_info(g_client);
        rc_client_user_game_summary_t s;
        rc_client_get_user_game_summary(g_client, &s);
        if (s.num_core_achievements == 0)
        {
            g_gameLine = "NONE YET";
            Say("RETROACHIEVEMENTS: " + Shown(game ? game->title : "", 30) + ": NO ACHIEVEMENTS YET");
        }
        else
        {
            UpdateGameLine();
            Say("RETROACHIEVEMENTS: " + std::to_string(s.num_unlocked_achievements) + " OF " +
                std::to_string(s.num_core_achievements) + " UNLOCKED");
        }
        return;
    }
    if (result == RC_NO_GAME_LOADED || result == RC_NOT_FOUND)
    {
        // (A ROM RetroAchievements does not know: another version, a hack without a set, or one changed.)
        g_gameLine = "NONE FOR THIS ROM";
        g_detail = "NOT A ROM IT KNOWS";
        Say("RETROACHIEVEMENTS: NO ACHIEVEMENTS FOR THIS ROM");
        return;
    }
    g_gameLine = "COULD NOT LOAD";
    g_detail = Shown(error, 30);
    Say("RETROACHIEVEMENTS: COULD NOT LOAD: " + g_detail);
}

void LoadGame()
{
    if (!g_client || !g_account || !g_account->on || g_loaded || g_loading || g_hash.empty()) return;
    if (!rc_client_get_user_info(g_client)) return; // (after the login)
    g_loading = true;
    g_gameLine.clear();
    rc_client_begin_load_game(g_client, g_hash.c_str(), GameLoaded, nullptr);
}

void RC_CCONV LoggedIn(int result, const char* error, rc_client_t*, void*)
{
    g_loggingIn = false;
    if (result == RC_OK)
    {
        const rc_client_user_t* u = rc_client_get_user_info(g_client);
        if (u && g_account)
        {
            g_account->user = u->username ? u->username : g_account->user;
            g_account->token = u->token ? u->token : "";
            g_loginError.clear();
            if (g_save) g_save();
            Say("RETROACHIEVEMENTS: LOGGED IN AS " + Shown(u->display_name ? u->display_name : u->username, 30));
        }
        LoadGame();
        return;
    }
    g_loginError = result == RC_INVALID_CREDENTIALS ? "WRONG USER NAME OR PASSWORD"
                 : result == RC_EXPIRED_TOKEN ? "LOGIN EXPIRED: LOG IN AGAIN" : Shown(error, 30);
    if (g_loginError.empty()) g_loginError = "ERROR " + std::to_string(result);
    // (A token the server no longer takes: logged out, to log in again with the password.)
    if ((result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) && g_account)
    {
        g_account->token.clear();
        if (g_save) g_save();
    }
    Say("RETROACHIEVEMENTS: LOGIN FAILED: " + g_loginError);
}

void LoginWithToken()
{
    if (!g_client || !g_account || !g_account->on || g_account->token.empty() || g_loggingIn) return;
    if (rc_client_get_user_info(g_client)) return;
    g_loggingIn = true;
    rc_client_begin_login_with_token(g_client, g_account->user.c_str(), g_account->token.c_str(), LoggedIn, nullptr);
}
}

// ---------------------------------------------------------------- the interface

void Start(Account* account, const std::string& product, std::function<void()> save)
{
    if (g_client) return;
    g_account = account;
    g_save = std::move(save);
    g_client = rc_client_create(ReadMemory, ServerCall);
    if (!g_client) return;
    // Casual only (achievements.h).
    rc_client_set_hardcore_enabled(g_client, 0);
    rc_client_set_allow_background_memory_reads(g_client, 0);
    rc_client_set_event_handler(g_client, Event);
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, Log);
    // "PokemonDSWidescreen/0.4.2 (Windows) rcheevos/12.5.0": the program as RetroAchievements
    // asks for it (its own name and version, never another emulator's).
    char clause[64] = {};
    rc_client_get_user_agent_clause(g_client, clause, sizeof clause);
    g_userAgent = product + " (" + SystemName() + ") " + clause;
    g_quit = false;
    g_worker = std::thread(Worker);
    LoginWithToken();
}

std::string Fingerprint(const std::vector<uint8_t>& rom)
{
    char hash[33] = {};
    if (rom.empty() || !rc_hash_generate_from_buffer(hash, RC_CONSOLE_NINTENDO_DS, rom.data(), rom.size())) return std::string();
    return hash;
}

void SetGame(melonDS::NDS* nds, const std::string& fingerprint)
{
    g_nds = nds;
    g_hash = fingerprint;
    g_loaded = false;
    g_gameLine.clear();
    fprintf(stderr, "[achievements] ROM fingerprint %s\n", g_hash.empty() ? "(none)" : g_hash.c_str());
    LoadGame();
}

bool Active()
{
    return g_client && g_account && g_account->on && g_loaded && !g_cheats;
}

void DoFrame()
{
    if (Active()) rc_client_do_frame(g_client);
}

void Pump(bool paused)
{
    if (!g_client) return;
    for (;;)
    {
        Response r;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (g_responses.empty()) break;
            r = std::move(g_responses.front());
            g_responses.pop_front();
        }
        rc_api_server_response_t a;
        memset(&a, 0, sizeof a);
        a.body = r.body.c_str();
        a.body_length = r.body.size();
        a.http_status_code = r.status;
        r.callback(&a, r.data);
    }
    // (Without frames, rc_client's timed work: pings for rich presence, unlocks sent again.)
    if ((paused || g_cheats) && g_account && g_account->on && rc_client_get_user_info(g_client)) rc_client_idle(g_client);
    UpdateGameLine();
}

void Reset()
{
    if (g_client && g_loaded) rc_client_reset(g_client);
}

void SetCheats(bool anyOn)
{
    if (anyOn == g_cheats) return;
    g_cheats = anyOn;
    if (g_loaded && g_account && g_account->on)
    {
        Say(anyOn ? "ACHIEVEMENTS PAUSED WHILE CHEATS ARE ON" : "ACHIEVEMENTS ON AGAIN");
        // (What was in progress while cheats were on does not count.)
        if (!anyOn) rc_client_reset(g_client);
    }
}

void SetOn(bool on)
{
    if (!g_account) return;
    g_account->on = on;
    if (g_save) g_save();
    if (!g_client) return;
    if (on)
    {
        LoginWithToken();
        LoadGame();
    }
    else if (g_loaded || g_loading)
    {
        rc_client_unload_game(g_client);
        g_loaded = g_loading = false;
        g_gameLine.clear();
    }
}

void Login(const std::string& user, const std::string& password)
{
    if (!g_client || g_loggingIn || user.empty() || password.empty()) return;
    if (rc_client_get_user_info(g_client)) Logout();
    // (The user name is kept only once the login works: LoggedIn.)
    g_loggingIn = true;
    g_loginError.clear();
    g_detail.clear();
    rc_client_begin_login_with_password(g_client, user.c_str(), password.c_str(), LoggedIn, nullptr);
}

void Logout()
{
    if (!g_client) return;
    if (g_loaded || g_loading) rc_client_unload_game(g_client);
    g_loaded = g_loading = false;
    rc_client_logout(g_client);
    if (g_account)
    {
        g_account->token.clear();
        if (g_save) g_save();
    }
    g_gameLine.clear();
}

std::string AccountLine()
{
    if (!g_client) return "-";
    if (g_loggingIn) return "LOGGING IN...";
    if (const rc_client_user_t* u = rc_client_get_user_info(g_client))
        return Shown(u->display_name ? u->display_name : u->username, 18);
    if (!g_loginError.empty()) return "LOGIN FAILED";
    if (g_account && !g_account->token.empty()) return Shown(g_account->user.c_str(), 18);
    return "NOT LOGGED IN";
}

std::string Detail()
{
    if (!g_loginError.empty() && !(g_client && rc_client_get_user_info(g_client))) return g_loginError;
    return g_detail;
}

bool LoggedIn()
{
    return g_account && !g_account->token.empty();
}

std::string GameLine()
{
    return g_gameLine.empty() ? "-" : g_gameLine;
}

bool Busy()
{
    return g_loggingIn || g_loading;
}

bool PopMessage(std::string& message)
{
    std::lock_guard<std::mutex> lock(g_msgMutex);
    if (g_messages.empty()) return false;
    message = std::move(g_messages.front());
    g_messages.pop_front();
    return true;
}

std::vector<Row> List()
{
    std::vector<Row> unlocked, locked;
    if (!g_client || !g_loaded) return unlocked;
    rc_client_achievement_list_t* list =
        rc_client_create_achievement_list(g_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return unlocked;
    for (uint32_t b = 0; b < list->num_buckets; b++)
        for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++)
        {
            const rc_client_achievement_t* a = list->buckets[b].achievements[i];
            if (!a) continue;
            Row r;
            r.title = a->title ? a->title : "";
            r.description = a->description ? a->description : "";
            r.progress = a->measured_progress;
            r.points = a->points;
            r.unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
            r.unlockTime = (int64_t)a->unlock_time;
            r.rarity = a->rarity;
            (r.unlocked ? unlocked : locked).push_back(std::move(r));
        }
    rc_client_destroy_achievement_list(list);
    unlocked.insert(unlocked.end(), locked.begin(), locked.end());
    return unlocked;
}

bool LoginForToken(const std::string& user, const std::string& password, const std::string& product,
                   std::string& outUser, std::string& outToken, std::string& error)
{
    if (user.empty() || password.empty())
    {
        error = "USER NAME AND PASSWORD NEEDED";
        return false;
    }
    rc_api_login_request_t params = {};
    params.username = user.c_str();
    params.password = password.c_str();
    rc_api_request_t request;
    if (rc_api_init_login_request(&request, &params) != RC_OK)
    {
        error = "COULD NOT MAKE THE REQUEST";
        return false;
    }
    int status = 0;
    std::string body;
    const bool sent = HttpPost(request.url, request.post_data ? request.post_data : "", request.content_type ? request.content_type : "",
                               product + " (" + SystemName() + ")", status, body, error);
    rc_api_destroy_request(&request);
    if (!sent)
    {
        if (error.empty()) error = "NO CONNECTION";
        return false;
    }
    rc_api_server_response_t answer = {};
    answer.body = body.c_str();
    answer.body_length = body.size();
    answer.http_status_code = status;
    rc_api_login_response_t response = {};
    const int rc = rc_api_process_login_server_response(&response, &answer);
    const bool ok = rc == RC_OK && response.response.succeeded && response.api_token && *response.api_token;
    if (ok)
    {
        outUser = response.username ? response.username : user;
        outToken = response.api_token;
    }
    else
        error = response.response.error_message && *response.response.error_message ? response.response.error_message
              : status ? "THE SERVER SAID NO (" + std::to_string(status) + ")" : "NO ANSWER";
    rc_api_destroy_login_response(&response);
    fprintf(stderr, "[achievements] login from the game list: %s\n", ok ? "done" : error.c_str());
    return ok;
}

void Stop()
{
    if (!g_client) return;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_quit = true;
        g_queueWake.notify_all();
    }
    // (A request on its way is left to finish on its own: the program is ending.)
    if (g_worker.joinable()) g_worker.detach();
    if (g_loaded) rc_client_unload_game(g_client);
    g_nds = nullptr;
    g_loaded = false;
    // (Not destroyed: the request thread may still hand it an answer. The program ends next.)
}

}
