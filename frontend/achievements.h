#pragma once
// RetroAchievements (retroachievements.org), casual mode only: the player's own account, the
// game's achievement set found by the ROM's fingerprint, unlocks sent as they happen. Built on
// RetroAchievements' rcheevos library (thirdparty/rcheevos, its rc_client); achievements.cpp ties
// it to the emulator, and the internet requests are achievements_http.cpp's (each platform's own).
//
// Off unless turned on (RETROACHIEVEMENTS in the pause menu). Never hardcore: the program offers
// save states, fast forward and cheats, which hardcore does not allow, and it is not one of the
// emulators RetroAchievements has checked for hardcore (the server would turn hardcore unlocks
// into casual ones anyway). The password is only sent to log in, never kept: the token the server
// gives back is kept in the settings file instead.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace melonDS { class NDS; }

namespace Achievements
{

// Whether this platform can reach the server yet (achievements_http.cpp): where it cannot, the
// pause menu has no RETROACHIEVEMENTS.
bool Available();

// What the settings file keeps (ra=, ra_user=, ra_token=).
struct Account
{
    bool on = false;
    std::string user, token;
};

// At start-up, before a game: the account (kept up to date as the player logs in and out, and
// saved with `save`), and the program's name and version as the server should know it
// ("PokemonDSWidescreen/0.4.2"). Logs in with the token if achievements are on.
void Start(Account* account, const std::string& product, std::function<void()> save);
// The ROM's fingerprint, by which RetroAchievements finds the game's set (empty: none). Taken
// before the ROM is let go of (the cartridge keeps its own copy).
std::string Fingerprint(const std::vector<uint8_t>& rom);
// The game, once its console exists and achievements have started: its fingerprint.
void SetGame(melonDS::NDS* nds, const std::string& fingerprint);
// Whether this game's achievements are being followed (on, logged in, a set found, no cheats).
bool Active();

// On the emulation's thread, after each emulated frame (each of fast forward's too).
void DoFrame();
// On the main thread, once per shown frame: the server's answers, and the library's own timed
// work while no frames are emulated (`paused`: the pause menu is open, or cheats are on).
void Pump(bool paused);
// The game was reset or a save state loaded: what was in progress starts again.
void Reset();
// Cheats on or off: achievements are not followed while any cheat is on.
void SetCheats(bool anyOn);

// The pause menu's RETROACHIEVEMENTS page.
void SetOn(bool on);
void Login(const std::string& user, const std::string& password);
void Logout();
std::string AccountLine(); // the user name, "NOT LOGGED IN", "LOGGING IN...", "LOGIN FAILED"
std::string GameLine();    // "12 OF 50 UNLOCKED", "NONE FOR THIS ROM", "LOADING...", ...
std::string Detail();      // what went wrong last (a login, the game's set), for a hint line
bool LoggedIn();           // an account is kept (its token)
// The game's achievements for the pause menu's ACHIEVEMENTS list: unlocked ones first, then the
// rest, each as the set has them (empty until the set is loaded).
struct Row
{
    std::string title, description, progress; // progress: "3/8" for one counted as it goes
    uint32_t points = 0;
    bool unlocked = false;
    int64_t unlockTime = 0; // when it was unlocked (seconds since 1970), 0: not known
    float rarity = 0;       // the share of players who have it, in percent (0: not known)
};
std::vector<Row> List();
bool Busy();               // a login or the game's set is on its way

// A login from the game list, with no game running (pc_launcher.cpp): the user name and password
// sent at once, the case-corrected name and the token given back, as the pause menu's LOG IN keeps
// them (ra_user=, ra_token=). Waits for the server's answer, so it is run off the main thread.
bool LoginForToken(const std::string& user, const std::string& password, const std::string& product,
                   std::string& outUser, std::string& outToken, std::string& error);

// Messages for the player (an unlock, a login that failed, ...), in the menus' font.
bool PopMessage(std::string& message);

void Stop();

}
