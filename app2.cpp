// app.cpp
// Compile: g++ app.cpp -o app.exe -municode -luser32
// Run as Administrator.

#include <windows.h>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <string>
#include <atomic>
#include <mutex>
#include <iostream>
#include <cctype>

using namespace std::chrono;

std::string morse_buffer;
std::mutex buf_mutex;

std::atomic_bool running(true);
std::atomic<long long> last_activity_ms(0);

// --- Konfigurasi ---
const bool TAP_MODE = true;          // jika true: tap '.' => '.' ; tap '-' => '-'
const int DOT_DASH_THRESHOLD = 300;  // ms, hanya dipakai kalau TAP_MODE == false
const int LETTER_TIMEOUT = 700;      // commit letter setelah tidak ada aktivitas (ms)
const int WORD_TIMEOUT = 1400;       // commit space setelah gap kata (ms)
// ---------------------

HHOOK g_hHook = NULL;
std::unordered_map<std::string, char> MORSE = {
    {".-", 'A'},{ "-...", 'B'},{ "-.-.", 'C'},{ "-..", 'D'},{".", 'E'},
    {"..-.", 'F'},{"--.", 'G'},{"....", 'H'},{"..", 'I'},{".---", 'J'},
    {"-.-", 'K'},{".-..", 'L'},{"--", 'M'},{"-.", 'N'},{"---", 'O'},
    {".--.", 'P'},{"--.-", 'Q'},{".-.", 'R'},{"...", 'S'},{"-", 'T'},
    {"..-", 'U'},{"...-", 'V'},{".--", 'W'},{"-..-", 'X'},{"-.--", 'Y'},
    {"--..", 'Z'},
    {"-----", '0'},{".----", '1'},{"..---", '2'},{"...--", '3'},{"....-", '4'},
    {".....", '5'},{"-....", '6'},{"--...", '7'},{"---..", '8'},{"----.", '9'}
};

// helper to send a single Unicode character via SendInput
void SendUnicodeChar(wchar_t ch) {
    INPUT ip[2];
    ZeroMemory(ip, sizeof(ip));
    ip[0].type = INPUT_KEYBOARD;
    ip[0].ki.wVk = 0;
    ip[0].ki.dwFlags = KEYEVENTF_UNICODE;
    ip[0].ki.wScan = ch;

    ip[1].type = INPUT_KEYBOARD;
    ip[1].ki.wVk = 0;
    ip[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    ip[1].ki.wScan = ch;

    SendInput(2, ip, sizeof(INPUT));
}

// Cek state CapsLock dan Shift; return true jika huruf harus uppercase
bool ShouldSendUppercase() {
    bool capsToggled = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return capsToggled ^ shiftPressed;
}

// commit the morse buffer: convert to char (if possible) and send with proper case
void CommitMorseBuffer() {
    std::lock_guard<std::mutex> lock(buf_mutex);
    if (morse_buffer.empty()) return;
    auto it = MORSE.find(morse_buffer);
    if (it != MORSE.end()) {
        char c = it->second;
        wchar_t wc;
        if (std::isalpha(static_cast<unsigned char>(c))) {
            bool uppercase = ShouldSendUppercase();
            char outc = uppercase ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                  : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            wc = static_cast<wchar_t>(outc);
        } else {
            wc = static_cast<wchar_t>(c);
        }
        SendUnicodeChar(wc);
    }
    morse_buffer.clear();
}

// background monitor untuk timeout (huruf / kata)
void MonitorThread() {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        long long last = last_activity_ms.load();
        if (last == 0) continue;
        long long now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        long long diff = now - last;
        if (!morse_buffer.empty()) {
            if (diff >= LETTER_TIMEOUT && diff < WORD_TIMEOUT) {
                CommitMorseBuffer();
                last_activity_ms.store(0);
            } else if (diff >= WORD_TIMEOUT) {
                CommitMorseBuffer();
                SendUnicodeChar(L' ');
                last_activity_ms.store(0);
            }
        }
    }
}

// menyimpan waktu keydown (dipakai kalau TAP_MODE == false)
static std::unordered_map<DWORD, long long> down_time;

// flag Left Ctrl pressed (if needed)
std::atomic_bool left_ctrl_down(false);

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        // Intercept hanya tombol '.' dan '-' pada keyboard utama:
        if (p->vkCode == VK_OEM_PERIOD || p->vkCode == VK_OEM_MINUS) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (!TAP_MODE) {
                    long long now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                    down_time[p->vkCode] = now;
                }
                // swallow keydown
                return 1;
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                char symbol = '.';
                if (TAP_MODE) {
                    if (p->vkCode == VK_OEM_PERIOD) symbol = '.';
                    else if (p->vkCode == VK_OEM_MINUS) symbol = '-';
                } else {
                    long long now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                    auto it = down_time.find(p->vkCode);
                    if (it != down_time.end()) {
                        long long down = it->second;
                        long long dur = now - down;
                        symbol = (dur < DOT_DASH_THRESHOLD) ? '.' : '-';
                        down_time.erase(it);
                    } else {
                        symbol = '.';
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(buf_mutex);
                    morse_buffer.push_back(symbol);
                    last_activity_ms.store(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
                }
                // swallow keyup
                return 1;
            }
        }

        // Intercept Left Ctrl (VK_LCONTROL) sebagai COMMIT key.
        if (p->vkCode == VK_LCONTROL) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                left_ctrl_down.store(true);
                // swallow the Ctrl down (do not pass through)
                return 1;
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                left_ctrl_down.store(false);
                // On release -> commit buffer immediately
                CommitMorseBuffer();
                // swallow the Ctrl up (do not pass through)
                return 1;
            }
        }

        // Esc untuk keluar (biarkan lewat tapi juga hentikan program)
        if (p->vkCode == VK_ESCAPE && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            running.store(false);
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::thread monitor(MonitorThread);

    g_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!g_hHook) {
        MessageBox(NULL, L"Failed to install keyboard hook. Run as Administrator.", L"Error", MB_ICONERROR);
        running.store(false);
        monitor.join();
        return 1;
    }

    // Informasi singkat
    std::wstring info = L"Morse hook active (TAP mode = ";
    info += (TAP_MODE ? L"true" : L"false");
    info += L").\n\nUse '.' and '-' keys to type Morse.\nPress Left Ctrl to commit the current letter immediately.\nPress Esc to exit.\n\nNote: Left Ctrl is used as commit key and will NOT function as modifier while program is active. Use Right Ctrl for shortcuts.";
    MessageBox(NULL, info.c_str(), L"Morse Hook", MB_OK);

    MSG msg;
    while (running.load() && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_hHook);
    running.store(false);
    monitor.join();
    return 0;
}
