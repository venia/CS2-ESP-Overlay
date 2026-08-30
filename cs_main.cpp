// cs_main.cpp
// ESP Overlay with Auto-Scanner
// Compiled: g++ -g cs_main.cpp -o cs_main.exe -lgdi32 -luser32 -lpsapi -static

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <cstring>

// ============================================
// STRUCTURES
// ============================================

struct Offsets {
    uintptr_t dwEntityList;
    uintptr_t dwLocalPlayerPawn;
    uintptr_t dwLocalPlayerController;
    uintptr_t dwViewMatrix;
    uintptr_t m_iHealth;
    uintptr_t m_iTeamNum;
    uintptr_t m_vecOrigin;
};

struct Vector3 {
    float x, y, z;
};

struct Vector2 {
    float x, y;
};

struct ViewMatrix {
    float m[4][4];
};

struct PlayerInfo {
    Vector3 position;
    int health;
    int team;
    bool isAlive;
};

// ============================================
// GLOBAL VARIABLES
// ============================================

HWND g_hOverlay = NULL;
bool g_running = true;
Offsets g_offsets = {};
bool g_offsetsFound = false;
int g_searchAttempts = 0;
HANDLE g_scannerProcess = NULL;

// ============================================
// SAFE FILE OPERATIONS (Atomic)
// ============================================

bool WriteFileSafe(const std::string& filename, const std::string& data) {
    std::string tempFile = filename + ".tmp";
    std::ofstream out(tempFile);
    if (!out.is_open()) return false;
    out << data;
    out.close();
    
    if (std::rename(tempFile.c_str(), filename.c_str()) != 0) {
        return false;
    }
    return true;
}

std::string ReadFileSafe(const std::string& filename) {
    std::ifstream in(filename);
    if (!in.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

void CreateDirectoryIfNotExists(const std::string& path) {
    CreateDirectoryA(path.c_str(), NULL);
}

void DeleteFileIfExists(const std::string& filename) {
    DeleteFileA(filename.c_str());
}

bool FileExists(const std::string& filename) {
    DWORD attrib = GetFileAttributesA(filename.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}

// ============================================
// BASE FUNCTIONS
// ============================================

DWORD GetProcessIdByName(const std::wstring& processName) {
    DWORD processId = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);
        
        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                if (_wcsicmp(processName.c_str(), processEntry.szExeFile) == 0) {
                    processId = processEntry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
    return processId;
}

uintptr_t GetModuleBaseAddress(DWORD pid, const std::wstring& moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W moduleEntry;
        moduleEntry.dwSize = sizeof(moduleEntry);
        
        if (Module32FirstW(snapshot, &moduleEntry)) {
            do {
                if (_wcsicmp(moduleName.c_str(), moduleEntry.szModule) == 0) {
                    CloseHandle(snapshot);
                    return (uintptr_t)moduleEntry.modBaseAddr;
                }
            } while (Module32NextW(snapshot, &moduleEntry));
        }
        CloseHandle(snapshot);
    }
    return 0;
}

template<typename T>
bool ReadMemory(HANDLE process, uintptr_t address, T& value) {
    SIZE_T bytesRead;
    return ReadProcessMemory(process, (LPCVOID)address, &value, sizeof(T), &bytesRead) 
           && bytesRead == sizeof(T);
}

bool IsValidAddress(uintptr_t address) {
    return address > 0x10000 && address < 0x7FFFFFFF0000;
}

// ============================================
// LAUNCH PATTERN SCANNER
// ============================================

bool LaunchPatternScanner() {
    std::cout << "[SCANNER] Launching pattern_scanning.exe..." << std::endl;
    
    // Get current directory
    char currentDir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, currentDir);
    std::string scannerPath = std::string(currentDir) + "\\pattern_scanning.exe";
    
    // Check if scanner exists
    if (!FileExists(scannerPath)) {
        std::cout << "[ERROR] pattern_scanning.exe not found!" << std::endl;
        std::cout << "       Expected path: " << scannerPath << std::endl;
        return false;
    }
    
    // Launch scanner as separate process (hidden window)
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // Hide the scanner window
    
    PROCESS_INFORMATION pi = {};
    
    std::string cmd = "\"" + scannerPath + "\"";
    
    if (CreateProcessA(
        NULL,
        (LPSTR)cmd.c_str(),
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,  // No window
        NULL,
        NULL,
        &si,
        &pi)) {
        
        g_scannerProcess = pi.hProcess;
        CloseHandle(pi.hThread);
        
        std::cout << "[SCANNER] Started (PID: " << pi.dwProcessId << ")" << std::endl;
        return true;
    }
    
    std::cout << "[ERROR] Failed to start scanner: " << GetLastError() << std::endl;
    return false;
}

bool IsScannerRunning() {
    if (g_scannerProcess == NULL) return false;
    DWORD exitCode;
    if (GetExitCodeProcess(g_scannerProcess, &exitCode)) {
        return (exitCode == STILL_ACTIVE);
    }
    return false;
}

void WaitForScanner() {
    if (g_scannerProcess == NULL) return;
    WaitForSingleObject(g_scannerProcess, INFINITE);
    CloseHandle(g_scannerProcess);
    g_scannerProcess = NULL;
}

// ============================================
// READ OFFSETS FROM poc.trs
// ============================================

bool ReadOffsetsFromFile(Offsets& offsets) {
    std::string jsonData = ReadFileSafe("secret/poc.trs");
    if (jsonData.empty()) return false;
    
    try {
        // Find status
        size_t statusPos = jsonData.find("\"status\":");
        if (statusPos != std::string::npos) {
            size_t endPos = jsonData.find(",", statusPos);
            std::string status = jsonData.substr(statusPos + 10, endPos - statusPos - 11);
            if (status != "found") return false;
        }
        
        // Find offsets
        size_t offsetsPos = jsonData.find("\"offsets\":");
        if (offsetsPos != std::string::npos) {
            auto findOffset = [&](const std::string& name) -> uintptr_t {
                size_t pos = jsonData.find("\"" + name + "\":");
                if (pos == std::string::npos) return 0;
                pos = jsonData.find(":", pos) + 1;
                while (jsonData[pos] == ' ') pos++;
                size_t endPos = jsonData.find_first_of(",}", pos);
                return std::stoull(jsonData.substr(pos, endPos - pos), nullptr, 10);
            };
            
            offsets.dwEntityList = findOffset("dwEntityList");
            offsets.dwLocalPlayerPawn = findOffset("dwLocalPlayerPawn");
            offsets.dwLocalPlayerController = findOffset("dwLocalPlayerController");
            offsets.dwViewMatrix = findOffset("dwViewMatrix");
            offsets.m_iHealth = findOffset("m_iHealth");
            offsets.m_iTeamNum = findOffset("m_iTeamNum");
            offsets.m_vecOrigin = findOffset("m_vecOrigin");
            
            return true;
        }
    } catch (...) {
        return false;
    }
    
    return false;
}

// ============================================
// GET PLAYERS
// ============================================

std::vector<PlayerInfo> GetPlayers(HANDLE hProcess, uintptr_t clientBase, Offsets offsets, PlayerInfo& localPlayer) {
    std::vector<PlayerInfo> players;
    
    uintptr_t entityList = 0;
    if (!ReadMemory(hProcess, clientBase + offsets.dwEntityList, entityList)) {
        return players;
    }
    
    if (!IsValidAddress(entityList)) {
        return players;
    }
    
    for (int i = 0; i < 64; i++) {
        uintptr_t playerPawn = 0;
        uintptr_t entityEntry = entityList + (i + 1) * 0x10;
        
        if (!ReadMemory(hProcess, entityEntry, playerPawn) || !IsValidAddress(playerPawn)) {
            continue;
        }
        
        int health = 0;
        if (!ReadMemory(hProcess, playerPawn + offsets.m_iHealth, health)) {
            continue;
        }
        
        if (health <= 0 || health > 100) {
            continue;
        }
        
        int team = 0;
        ReadMemory(hProcess, playerPawn + offsets.m_iTeamNum, team);
        
        if (team != 2 && team != 3) {
            continue;
        }
        
        PlayerInfo player = {};
        player.health = health;
        player.team = team;
        player.isAlive = true;
        
        if (offsets.m_vecOrigin == 0x80) {
            uintptr_t sceneNode = 0;
            ReadMemory(hProcess, playerPawn + 0x330, sceneNode);
            if (IsValidAddress(sceneNode)) {
                ReadMemory(hProcess, sceneNode + 0x80, player.position);
            }
        } else {
            ReadMemory(hProcess, playerPawn + offsets.m_vecOrigin, player.position);
        }
        
        players.push_back(player);
    }
    
    return players;
}

// ============================================
// RENDERING FUNCTIONS
// ============================================

bool WorldToScreen(Vector3 worldPos, Vector2& screenPos, ViewMatrix vm, int screenWidth, int screenHeight) {
    float clipX = worldPos.x * vm.m[0][0] + worldPos.y * vm.m[1][0] + worldPos.z * vm.m[2][0] + vm.m[3][0];
    float clipY = worldPos.x * vm.m[0][1] + worldPos.y * vm.m[1][1] + worldPos.z * vm.m[2][1] + vm.m[3][1];
    float clipZ = worldPos.x * vm.m[0][2] + worldPos.y * vm.m[1][2] + worldPos.z * vm.m[2][2] + vm.m[3][2];
    float clipW = worldPos.x * vm.m[0][3] + worldPos.y * vm.m[1][3] + worldPos.z * vm.m[2][3] + vm.m[3][3];
    
    if (clipW < 0.1f) return false;
    
    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;
    
    screenPos.x = (screenWidth / 2.0f) * (ndcX + 1.0f);
    screenPos.y = (screenHeight / 2.0f) * (1.0f - ndcY);
    
    return true;
}

void DrawBox(HDC hdc, Vector2 screenPos, int width, int height, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    
    Rectangle(hdc, 
        (int)(screenPos.x - width / 2), 
        (int)(screenPos.y - height), 
        (int)(screenPos.x + width / 2), 
        (int)screenPos.y
    );
    
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

void DrawHealthBar(HDC hdc, Vector2 screenPos, int health, int width, int height) {
    float healthPercent = health / 100.0f;
    int barHeight = (int)(height * healthPercent);
    
    COLORREF color;
    if (health > 70) color = RGB(0, 255, 0);
    else if (health > 40) color = RGB(255, 255, 0);
    else color = RGB(255, 0, 0);
    
    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    
    Rectangle(hdc,
        (int)(screenPos.x - width / 2 - 8),
        (int)(screenPos.y - height + (height - barHeight)),
        (int)(screenPos.x - width / 2 - 4),
        (int)screenPos.y
    );
    
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void DrawText(HDC hdc, Vector2 screenPos, const char* text, COLORREF color, int height) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    
    HFONT font = CreateFont(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, (int)screenPos.x - 1, (int)screenPos.y - height - 16, text, (int)strlen(text));
    TextOutA(hdc, (int)screenPos.x + 1, (int)screenPos.y - height - 16, text, (int)strlen(text));
    TextOutA(hdc, (int)screenPos.x, (int)screenPos.y - height - 17, text, (int)strlen(text));
    TextOutA(hdc, (int)screenPos.x, (int)screenPos.y - height - 15, text, (int)strlen(text));
    
    SetTextColor(hdc, color);
    TextOutA(hdc, (int)screenPos.x, (int)screenPos.y - height - 16, text, (int)strlen(text));
    
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void DrawESP(HDC hdc, const std::vector<PlayerInfo>& players, const PlayerInfo& localPlayer, 
             ViewMatrix vm, int screenWidth, int screenHeight, bool offsetsFound, int attempts) {
    
    // ============================================
    // HEADER INFO
    // ============================================
    
    SetTextColor(hdc, RGB(0, 255, 0));
    SetBkMode(hdc, TRANSPARENT);
    
    HFONT debugFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    
    HFONT oldFont = (HFONT)SelectObject(hdc, debugFont);
    
    // Shadow
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, 9, 9, "ESP ACTIVE", 10);
    TextOutA(hdc, 11, 9, "ESP ACTIVE", 10);
    TextOutA(hdc, 10, 8, "ESP ACTIVE", 10);
    TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
    
    if (offsetsFound) {
        SetTextColor(hdc, RGB(0, 255, 0));
        TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
        
        char foundText[64];
        sprintf(foundText, "[OK] Offsets Found! (%d attempts)", attempts);
        SetTextColor(hdc, RGB(0, 255, 0));
        TextOutA(hdc, 10, 35, foundText, (int)strlen(foundText));
    } else {
        SetTextColor(hdc, RGB(255, 255, 0));
        TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
        
        char searchingText[64];
        sprintf(searchingText, "[SCAN] Searching for offsets... (%d attempts)", attempts);
        SetTextColor(hdc, RGB(255, 255, 0));
        TextOutA(hdc, 10, 35, searchingText, (int)strlen(searchingText));
        
        // Show scanner status
        if (g_scannerProcess != NULL) {
            DWORD exitCode;
            if (GetExitCodeProcess(g_scannerProcess, &exitCode)) {
                if (exitCode == STILL_ACTIVE) {
                    SetTextColor(hdc, RGB(0, 255, 255));
                    TextOutA(hdc, 10, 60, "[SCANNER] Running...", 20);
                }
            }
        }
    }
    
    char infoText[256];
    sprintf(infoText, "Players: %d | HP: %d | Team: %d", 
            (int)players.size(), localPlayer.health, localPlayer.team);
    
    int yOffset = offsetsFound ? 60 : 85;
    SetTextColor(hdc, RGB(0, 255, 255));
    TextOutA(hdc, 10, yOffset, infoText, (int)strlen(infoText));
    
    char posText[256];
    sprintf(posText, "Pos: (%.1f, %.1f, %.1f)", 
            localPlayer.position.x, localPlayer.position.y, localPlayer.position.z);
    
    SetTextColor(hdc, RGB(255, 255, 0));
    TextOutA(hdc, 10, yOffset + 25, posText, (int)strlen(posText));
    
    SelectObject(hdc, oldFont);
    DeleteObject(debugFont);
    
    // ============================================
    // PLAYER ESP (only if offsets found)
    // ============================================
    
    if (!offsetsFound) return;
    
    for (const auto& player : players) {
        if (player.position.x == localPlayer.position.x && 
            player.position.y == localPlayer.position.y &&
            player.position.z == localPlayer.position.z) {
            continue;
        }
        
        if (!player.isAlive) continue;
        
        Vector2 screenPos;
        if (!WorldToScreen(player.position, screenPos, vm, screenWidth, screenHeight)) {
            continue;
        }
        
        bool isEnemy = (player.team != localPlayer.team);
        COLORREF color = isEnemy ? RGB(255, 0, 0) : RGB(0, 255, 0);
        
        int boxWidth = 30;
        int boxHeight = 70;
        
        DrawBox(hdc, screenPos, boxWidth, boxHeight, color);
        DrawHealthBar(hdc, screenPos, player.health, boxWidth, boxHeight);
        
        char text[32];
        sprintf(text, "%d HP", player.health);
        DrawText(hdc, screenPos, text, color, boxHeight);
    }
}

// ============================================
// OVERLAY WINDOW
// ============================================

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_running = false;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
            return 0;
        }
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

HWND CreateOverlay(int width, int height) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "OverlayClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    
    if (!RegisterClassA(&wc)) {
        std::cout << "[ERROR] Failed to register window class!" << std::endl;
        return NULL;
    }
    
    HWND hGame = FindWindowA(NULL, "Counter-Strike 2");
    if (!hGame) {
        std::cout << "[ERROR] CS2 window not found!" << std::endl;
        return NULL;
    }
    
    RECT gameRect;
    GetWindowRect(hGame, &gameRect);
    
    HWND hOverlay = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        "OverlayClass",
        "Overlay",
        WS_POPUP,
        gameRect.left,
        gameRect.top,
        gameRect.right - gameRect.left,
        gameRect.bottom - gameRect.top,
        NULL,
        NULL,
        GetModuleHandleA(NULL),
        NULL
    );
    
    if (!hOverlay) {
        std::cout << "[ERROR] Failed to create overlay!" << std::endl;
        return NULL;
    }
    
    SetLayeredWindowAttributes(hOverlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
    SetWindowLong(hOverlay, GWL_EXSTYLE, 
        GetWindowLong(hOverlay, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    
    ShowWindow(hOverlay, SW_SHOW);
    UpdateWindow(hOverlay);
    
    std::cout << "[OK] Overlay created!" << std::endl;
    return hOverlay;
}

void UpdateOverlayPosition(HWND hOverlay) {
    HWND hGame = FindWindowA(NULL, "Counter-Strike 2");
    if (!hGame) return;
    
    RECT gameRect;
    GetWindowRect(hGame, &gameRect);
    
    SetWindowPos(hOverlay, HWND_TOPMOST,
        gameRect.left,
        gameRect.top,
        gameRect.right - gameRect.left,
        gameRect.bottom - gameRect.top,
        SWP_NOZORDER | SWP_SHOWWINDOW
    );
}

ViewMatrix GetViewMatrixFromMemory(HANDLE hProcess, uintptr_t clientBase, uintptr_t viewMatrixOffset) {
    ViewMatrix vm = {};
    ReadMemory(hProcess, clientBase + viewMatrixOffset, vm);
    return vm;
}

// ============================================
// MAIN
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║           CS2 ESP OVERLAY with Auto-Scanner                ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    // 1. Create secret folder and clean up
    CreateDirectoryIfNotExists("secret");
    DeleteFileIfExists("secret/ccs.trs");
    
    // 2. Find CS2
    DWORD pid = GetProcessIdByName(L"cs2.exe");
    if (pid == 0) {
        std::cout << "[ERROR] CS2 is not running!" << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] CS2 PID: " << pid << std::endl;

    // 3. Open process
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        std::cout << "[ERROR] Failed to open process! Run as Administrator." << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] Process opened" << std::endl;

    // 4. Get client.dll base
    uintptr_t clientBase = GetModuleBaseAddress(pid, L"client.dll");
    if (clientBase == 0) {
        std::cout << "[ERROR] client.dll not found!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] client.dll: 0x" << std::hex << clientBase << std::dec << std::endl;

    // 5. Launch pattern scanner (as child process)
    std::cout << std::endl;
    if (!LaunchPatternScanner()) {
        std::cout << "[WARNING] Scanner failed to start. Using fallback offsets." << std::endl;
    }
    std::cout << std::endl;

    // 6. Create overlay
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    g_hOverlay = CreateOverlay(screenWidth, screenHeight);
    if (!g_hOverlay) {
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] Overlay created" << std::endl;

    std::cout << std::endl << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  ESP RUNNING - Press ESC to exit" << std::endl;
    std::cout << "  Scanner is searching for offsets in background" << std::endl;
    std::cout << "  Take damage in game to help the scanner!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    // 7. Fallback offsets
    Offsets fallbackOffsets = {};
    fallbackOffsets.dwEntityList = 0x2571220;
    fallbackOffsets.dwLocalPlayerPawn = 0x23C6268;
    fallbackOffsets.dwLocalPlayerController = 0x23A0F30;
    fallbackOffsets.dwViewMatrix = 0x23CB830;
    fallbackOffsets.m_iHealth = 0x34C;
    fallbackOffsets.m_iTeamNum = 0x3E7;
    fallbackOffsets.m_vecOrigin = 0x80;
    
    g_offsets = fallbackOffsets;
    
    // 8. Main loop
    MSG msg = {};
    auto lastWriteTime = std::chrono::steady_clock::now();
    auto lastScannerCheck = std::chrono::steady_clock::now();
    
    while (g_running) {
        // Handle Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
        }
        
        if (!g_running) break;
        
        // Update overlay position
        UpdateOverlayPosition(g_hOverlay);
        
        // Read local player
        uintptr_t localPlayerPawn = 0;
        int localHealth = 0;
        int localTeam = 0;
        Vector3 localPos = {0, 0, 0};
        
        if (g_offsetsFound) {
            ReadMemory(hProcess, clientBase + g_offsets.dwLocalPlayerPawn, localPlayerPawn);
            if (IsValidAddress(localPlayerPawn)) {
                ReadMemory(hProcess, localPlayerPawn + g_offsets.m_iHealth, localHealth);
                ReadMemory(hProcess, localPlayerPawn + g_offsets.m_iTeamNum, localTeam);
                
                if (g_offsets.m_vecOrigin == 0x80) {
                    uintptr_t sceneNode = 0;
                    ReadMemory(hProcess, localPlayerPawn + 0x330, sceneNode);
                    if (IsValidAddress(sceneNode)) {
                        ReadMemory(hProcess, sceneNode + 0x80, localPos);
                    }
                } else {
                    ReadMemory(hProcess, localPlayerPawn + g_offsets.m_vecOrigin, localPos);
                }
            }
        } else {
            ReadMemory(hProcess, clientBase + fallbackOffsets.dwLocalPlayerPawn, localPlayerPawn);
            if (IsValidAddress(localPlayerPawn)) {
                ReadMemory(hProcess, localPlayerPawn + fallbackOffsets.m_iHealth, localHealth);
                ReadMemory(hProcess, localPlayerPawn + fallbackOffsets.m_iTeamNum, localTeam);
                
                if (fallbackOffsets.m_vecOrigin == 0x80) {
                    uintptr_t sceneNode = 0;
                    ReadMemory(hProcess, localPlayerPawn + 0x330, sceneNode);
                    if (IsValidAddress(sceneNode)) {
                        ReadMemory(hProcess, sceneNode + 0x80, localPos);
                    }
                } else {
                    ReadMemory(hProcess, localPlayerPawn + fallbackOffsets.m_vecOrigin, localPos);
                }
            }
        }
        
        PlayerInfo localPlayer = {};
        localPlayer.health = localHealth;
        localPlayer.team = localTeam;
        localPlayer.position = localPos;
        localPlayer.isAlive = (localHealth > 0 && localHealth <= 100);
        
        // Get players and view matrix
        std::vector<PlayerInfo> players;
        ViewMatrix vm = {};
        
        if (g_offsetsFound) {
            vm = GetViewMatrixFromMemory(hProcess, clientBase, g_offsets.dwViewMatrix);
            players = GetPlayers(hProcess, clientBase, g_offsets, localPlayer);
        } else {
            vm = GetViewMatrixFromMemory(hProcess, clientBase, fallbackOffsets.dwViewMatrix);
            players = GetPlayers(hProcess, clientBase, fallbackOffsets, localPlayer);
        }
        
        // ============================================
        // WRITE TO secret/ccs.trs (every 500ms)
        // ============================================
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWriteTime).count() >= 500) {
            lastWriteTime = now;
            
            std::stringstream json;
            json << "{\n";
            json << "    \"timestamp\": \"" << std::chrono::system_clock::now().time_since_epoch().count() << "\",\n";
            json << "    \"health\": " << localHealth << ",\n";
            json << "    \"team\": " << localTeam << ",\n";
            json << "    \"position\": {\n";
            json << "        \"x\": " << localPos.x << ",\n";
            json << "        \"y\": " << localPos.y << ",\n";
            json << "        \"z\": " << localPos.z << "\n";
            json << "    },\n";
            json << "    \"search\": {\n";
            json << "        \"attempts\": " << g_searchAttempts << ",\n";
            json << "        \"status\": \"" << (g_offsetsFound ? "found" : "scanning") << "\",\n";
            json << "        \"candidates\": " << (g_offsetsFound ? 1 : 0) << "\n";
            json << "    }\n";
            json << "}";
            
            WriteFileSafe("secret/ccs.trs", json.str());
            g_searchAttempts++;
        }
        
        // ============================================
        // CHECK FOR poc.trs (offsets found)
        // ============================================
        if (!g_offsetsFound) {
            Offsets newOffsets = {};
            if (ReadOffsetsFromFile(newOffsets)) {
                if (newOffsets.m_iHealth != 0) {
                    g_offsets = newOffsets;
                    g_offsetsFound = true;
                    std::cout << std::endl;
                    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
                    std::cout << "║                    OFFSETS FOUND!                         ║" << std::endl;
                    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;
                    std::cout << "  m_iHealth      : 0x" << std::hex << g_offsets.m_iHealth << std::dec << std::endl;
                    std::cout << "  dwEntityList   : 0x" << std::hex << g_offsets.dwEntityList << std::dec << std::endl;
                    std::cout << "  dwViewMatrix   : 0x" << std::hex << g_offsets.dwViewMatrix << std::dec << std::endl;
                    std::cout << "  ESP is now fully functional!" << std::endl;
                    std::cout << std::endl;
                }
            }
        }
        
        // ============================================
        // RENDER ESP
        // ============================================
        HDC hdc = GetDC(g_hOverlay);
        
        RECT rect;
        GetClientRect(g_hOverlay, &rect);
        HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rect, clearBrush);
        DeleteObject(clearBrush);
        
        DrawESP(hdc, players, localPlayer, vm, screenWidth, screenHeight, 
                g_offsetsFound, g_searchAttempts);
        
        ReleaseDC(g_hOverlay, hdc);
        
        Sleep(16);
    }
    
    // ============================================
    // CLEANUP
    // ============================================
    
    // Delete only ccs.trs, keep poc.trs for debugging
    DeleteFileIfExists("secret/ccs.trs");
    
    // Wait for scanner to finish if still running
    if (g_scannerProcess != NULL) {
        std::cout << "Waiting for scanner to finish..." << std::endl;
        WaitForScanner();
    }
    
    if (g_hOverlay) {
        DestroyWindow(g_hOverlay);
        g_hOverlay = NULL;
    }
    
    CloseHandle(hProcess);
    std::cout << "ESP stopped." << std::endl;
    return 0;
}