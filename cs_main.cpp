#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

// ============================================
// СТРУКТУРЫ
// ============================================

struct Offsets {
    uintptr_t dwEntityList;
    uintptr_t dwLocalPlayerPawn;
    uintptr_t dwViewMatrix;
    uintptr_t m_iHealth;
    uintptr_t m_iTeamNum;
    uintptr_t m_lifeState;
    uintptr_t m_fFlags;
    uintptr_t m_vecOrigin;
    uintptr_t dwGameEntitySystem;
    uintptr_t dwGameEntitySystem_highestEntityIndex;
    uintptr_t dwLocalPlayerController;
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
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================

HWND g_hOverlay = NULL;
bool g_running = true;

// ============================================
// ПРОТОТИПЫ ФУНКЦИЙ
// ============================================

template<typename T>
bool ReadMemory(HANDLE process, uintptr_t address, T& value);
bool IsValidAddress(uintptr_t address);
bool WorldToScreen(Vector3 worldPos, Vector2& screenPos, ViewMatrix vm, int screenWidth, int screenHeight);

// ============================================
// ФУНКЦИИ РИСОВАНИЯ
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
             ViewMatrix vm, int screenWidth, int screenHeight) {
    
    // Отладочная информация
    SetTextColor(hdc, RGB(0, 255, 0));
    SetBkMode(hdc, TRANSPARENT);
    
    HFONT debugFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    
    HFONT oldFont = (HFONT)SelectObject(hdc, debugFont);
    
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, 9, 9, "ESP ACTIVE", 10);
    TextOutA(hdc, 11, 9, "ESP ACTIVE", 10);
    TextOutA(hdc, 10, 8, "ESP ACTIVE", 10);
    TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
    
    SetTextColor(hdc, RGB(0, 255, 0));
    TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
    
    char infoText[256];
    sprintf(infoText, "Players: %d | Local HP: %d | Team: %d", 
            (int)players.size(), localPlayer.health, localPlayer.team);
    
    SetTextColor(hdc, RGB(0, 255, 255));
    TextOutA(hdc, 10, 40, infoText, (int)strlen(infoText));
    
    char posText[256];
    sprintf(posText, "Pos: (%.1f, %.1f, %.1f)", 
            localPlayer.position.x, localPlayer.position.y, localPlayer.position.z);
    
    SetTextColor(hdc, RGB(255, 255, 0));
    TextOutA(hdc, 10, 65, posText, (int)strlen(posText));
    
    // Тестовый квадрат в центре
    HPEN testPen = CreatePen(PS_SOLID, 3, RGB(255, 0, 255));
    HPEN oldPen2 = (HPEN)SelectObject(hdc, testPen);
    HBRUSH oldBrush2 = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, screenWidth/2 - 50, screenHeight/2 - 50, screenWidth/2 + 50, screenHeight/2 + 50);
    SelectObject(hdc, oldPen2);
    SelectObject(hdc, oldBrush2);
    DeleteObject(testPen);
    
    SelectObject(hdc, oldFont);
    DeleteObject(debugFont);
    
    // Рисуем игроков
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
// ОКНО ОВЕРЛЕЯ
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
        std::cout << "[!] Не удалось зарегистрировать класс окна!" << std::endl;
        return NULL;
    }
    
    HWND hGame = FindWindowA(NULL, "Counter-Strike 2");
    if (!hGame) {
        std::cout << "[!] Окно CS2 не найдено!" << std::endl;
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
        std::cout << "[!] Не удалось создать оверлей!" << std::endl;
        return NULL;
    }
    
    SetLayeredWindowAttributes(hOverlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
    SetWindowLong(hOverlay, GWL_EXSTYLE, 
        GetWindowLong(hOverlay, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    
    ShowWindow(hOverlay, SW_SHOW);
    UpdateWindow(hOverlay);
    
    std::cout << "   [✓] Оверлей создан!" << std::endl;
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

// ============================================
// ПАРСИНГ СМЕЩЕНИЙ
// ============================================

ViewMatrix GetViewMatrixFromMemory(HANDLE hProcess, uintptr_t clientBase, uintptr_t viewMatrixOffset) {
    ViewMatrix vm = {};
    ReadMemory(hProcess, clientBase + viewMatrixOffset, vm);
    return vm;
}

bool RunCS2Dumper() {
    std::string cmd = "cs2-dumper.exe --output ./output";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    
    if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

uintptr_t FindOffsetInFile(const std::string& filepath, const std::string& offsetName, const std::string& namespaceName) {
    std::ifstream file(filepath);
    std::string line;
    std::regex pattern(R"(constexpr std::ptrdiff_t (\w+) = (0x[0-9A-Fa-f]+);)");
    
    bool inNamespace = false;
    int braceDepth = 0;
    
    while (std::getline(file, line)) {
        if (line.find("namespace " + namespaceName + " {") != std::string::npos) {
            inNamespace = true;
            braceDepth = 1;
            continue;
        }
        
        if (inNamespace) {
            for (char c : line) {
                if (c == '{') braceDepth++;
                if (c == '}') braceDepth--;
            }
            
            if (braceDepth == 0) {
                inNamespace = false;
                continue;
            }
            
            std::smatch match;
            if (std::regex_search(line, match, pattern)) {
                std::string name = match[1].str();
                if (name == offsetName) {
                    std::string value = match[2].str();
                    return std::stoull(value, nullptr, 16);
                }
            }
        }
    }
    
    return 0;
}

Offsets ParseOffsets(const std::string& filepath) {
    Offsets offsets = {};
    std::ifstream file(filepath);
    std::string line;
    std::regex pattern(R"(constexpr std::ptrdiff_t (\w+) = (0x[0-9A-Fa-f]+);)");
    
    bool inClientDll = false;
    
    while (std::getline(file, line)) {
        if (line.find("namespace client_dll {") != std::string::npos) {
            inClientDll = true;
            continue;
        }
        if (line.find("}") != std::string::npos && inClientDll) {
            inClientDll = false;
            continue;
        }
        
        if (inClientDll) {
            std::smatch match;
            if (std::regex_search(line, match, pattern)) {
                std::string name = match[1].str();
                std::string value = match[2].str();
                uintptr_t addr = std::stoull(value, nullptr, 16);
                
                if (name == "dwEntityList") offsets.dwEntityList = addr;
                else if (name == "dwLocalPlayerPawn") offsets.dwLocalPlayerPawn = addr;
                else if (name == "dwLocalPlayerController") offsets.dwLocalPlayerController = addr;
                else if (name == "dwViewMatrix") offsets.dwViewMatrix = addr;
                else if (name == "dwGameEntitySystem") offsets.dwGameEntitySystem = addr;
                else if (name == "dwGameEntitySystem_highestEntityIndex") offsets.dwGameEntitySystem_highestEntityIndex = addr;
            }
        }
    }
    
    return offsets;
}

Offsets GetOffsets() {
    Offsets offsets = {};
    
    std::cout << "Запуск cs2-dumper для получения актуальных смещений..." << std::endl;
    
    CreateDirectoryA("./output", NULL);
    
    if (!RunCS2Dumper()) {
        std::cout << "   [!] Не удалось запустить cs2-dumper!" << std::endl;
        return offsets;
    }
    
    offsets = ParseOffsets("./output/offsets.hpp");
    if (offsets.dwEntityList == 0) {
        std::cout << "   [!] Не удалось прочитать offsets.hpp!" << std::endl;
        return offsets;
    }
    
    offsets.m_iHealth = FindOffsetInFile("./output/client_dll.hpp", "m_iHealth", "C_BaseEntity");
    offsets.m_iTeamNum = FindOffsetInFile("./output/client_dll.hpp", "m_iTeamNum", "C_BaseEntity");
    offsets.m_lifeState = FindOffsetInFile("./output/client_dll.hpp", "m_lifeState", "C_BaseEntity");
    offsets.m_fFlags = FindOffsetInFile("./output/client_dll.hpp", "m_fFlags", "C_BaseEntity");
    
    uintptr_t vecOriginInSceneNode = FindOffsetInFile("./output/client_dll.hpp", "m_vecOrigin", "CGameSceneNode");
    uintptr_t vecOriginInBaseEntity = FindOffsetInFile("./output/client_dll.hpp", "m_vecOrigin", "C_BaseEntity");
    
    if (vecOriginInBaseEntity != 0) {
        offsets.m_vecOrigin = vecOriginInBaseEntity;
    } else if (vecOriginInSceneNode != 0) {
        offsets.m_vecOrigin = vecOriginInSceneNode;
    }
    
    std::cout << "   [✓] Смещения получены!" << std::endl;
    std::cout << "   dwEntityList: 0x" << std::hex << offsets.dwEntityList << std::dec << std::endl;
    std::cout << "   dwLocalPlayerPawn: 0x" << std::hex << offsets.dwLocalPlayerPawn << std::dec << std::endl;
    std::cout << "   dwLocalPlayerController: 0x" << std::hex << offsets.dwLocalPlayerController << std::dec << std::endl;
    std::cout << "   dwViewMatrix: 0x" << std::hex << offsets.dwViewMatrix << std::dec << std::endl;
    std::cout << "   dwGameEntitySystem: 0x" << std::hex << offsets.dwGameEntitySystem << std::dec << std::endl;
    std::cout << "   m_iHealth: 0x" << std::hex << offsets.m_iHealth << std::dec << std::endl;
    std::cout << "   m_iTeamNum: 0x" << std::hex << offsets.m_iTeamNum << std::dec << std::endl;
    std::cout << "   m_vecOrigin: 0x" << std::hex << offsets.m_vecOrigin << std::dec << std::endl;
    
    return offsets;
}

// ============================================
// БАЗОВЫЕ ФУНКЦИИ
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
// ФУНКЦИЯ ДЛЯ ПОЛУЧЕНИЯ ИГРОКОВ
// ============================================

std::vector<PlayerInfo> GetPlayers(HANDLE hProcess, uintptr_t clientBase, Offsets offsets, PlayerInfo& localPlayer) {
    std::vector<PlayerInfo> players;
    
    // 1. Читаем GameEntitySystem
    uintptr_t entitySystem = 0;
    if (!ReadMemory(hProcess, clientBase + offsets.dwGameEntitySystem, entitySystem)) {
        return players;
    }
    
    if (!IsValidAddress(entitySystem)) {
        return players;
    }
    
    // 2. Читаем highestIndex
    int highestIndex = 0;
    ReadMemory(hProcess, entitySystem + 0x2090, highestIndex);
    
    if (highestIndex == 0 || highestIndex > 10000) {
        highestIndex = 512;
    }
    
    static int debugCount = 0;
    if (debugCount++ % 60 == 0) {
        std::cout << "[DEBUG] entitySystem: 0x" << std::hex << entitySystem 
                  << " | highestIndex: " << std::dec << highestIndex << std::endl;
    }
    
    // 3. Проходим по сущностям
    int validPlayers = 0;
    int team2Count = 0;
    int team3Count = 0;
    
    for (int i = 0; i < highestIndex; i++) {
        uintptr_t listEntry = 0;
        if (!ReadMemory(hProcess, entitySystem + 0x18 + i * 0x8, listEntry)) {
            continue;
        }
        
        if (!IsValidAddress(listEntry)) {
            continue;
        }
        
        uintptr_t entity = 0;
        if (!ReadMemory(hProcess, listEntry + 0x0, entity)) {
            continue;
        }
        
        if (!IsValidAddress(entity)) {
            continue;
        }
        
        // Читаем здоровье
        int health = 0;
        if (!ReadMemory(hProcess, entity + offsets.m_iHealth, health)) {
            continue;
        }
        
        if (health <= 0 || health > 100) {
            continue;
        }
        
        // ЧИТАЕМ КОМАНДУ ЧЕРЕЗ CONTROLLER
        int team = 0;
        
        // Смещение m_hController в C_BasePlayerPawn = 0x13D0
        uintptr_t controllerHandle = 0;
        if (ReadMemory(hProcess, entity + 0x13D0, controllerHandle)) {
            if (controllerHandle != 0) {
                int controllerIndex = controllerHandle & 0x7FFF;
                
                if (controllerIndex > 0 && controllerIndex < 10000) {
                    uintptr_t entityList = 0;
                    if (ReadMemory(hProcess, clientBase + offsets.dwEntityList, entityList)) {
                        if (IsValidAddress(entityList)) {
                            uintptr_t controllerEntry = entityList + controllerIndex * 0x10;
                            uintptr_t controller = 0;
                            if (ReadMemory(hProcess, controllerEntry, controller)) {
                                if (IsValidAddress(controller)) {
                                    if (ReadMemory(hProcess, controller + offsets.m_iTeamNum, team)) {
                                        // team = 2 или 3 для игроков
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Если не получилось через Controller, пробуем через саму сущность
        if (team == 0) {
            ReadMemory(hProcess, entity + offsets.m_iTeamNum, team);
        }
        
        // Если команда все еще 0 - пропускаем (не игрок)
        if (team == 0) {
            continue;
        }
        
        // Только игроки (команда 2 или 3)
        if (team != 2 && team != 3) {
            continue;
        }
        
        if (team == 2) team2Count++;
        if (team == 3) team3Count++;
        
        PlayerInfo player = {};
        player.health = health;
        player.team = team;
        player.isAlive = true;
        
        // Читаем позицию
        if (offsets.m_vecOrigin == 0x80) {
            uintptr_t sceneNode = 0;
            ReadMemory(hProcess, entity + 0x330, sceneNode);
            if (IsValidAddress(sceneNode)) {
                ReadMemory(hProcess, sceneNode + 0x80, player.position);
            }
        } else {
            ReadMemory(hProcess, entity + offsets.m_vecOrigin, player.position);
        }
        
        // Проверяем, не локальный ли это игрок
        if (player.position.x == localPlayer.position.x && 
            player.position.y == localPlayer.position.y &&
            player.position.z == localPlayer.position.z) {
            continue;
        }
        
        players.push_back(player);
        validPlayers++;
    }
    
    if (debugCount % 60 == 0) {
        std::cout << "[DEBUG] Team 2: " << team2Count 
                  << " | Team 3: " << team3Count 
                  << " | Valid players: " << validPlayers << std::endl;
    }
    
    return players;
}

// ============================================
// ОСНОВНАЯ ПРОГРАММА
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=== CS2 ESP Overlay ===" << std::endl << std::endl;
    
    // 1. Получаем смещения
    Offsets localOffsets = GetOffsets();
    if (localOffsets.dwEntityList == 0 || localOffsets.m_vecOrigin == 0) {
        std::cout << "[!] Не удалось получить смещения!" << std::endl;
        std::cout << "Нажми Enter для выхода...";
        std::cin.get();
        return 1;
    }

    // 2. Находим CS2
    DWORD pid = GetProcessIdByName(L"cs2.exe");
    if (pid == 0) {
        std::cout << "[!] CS2 не запущена!" << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "1. [✓] CS2 PID: " << pid << std::endl;

    // 3. Открываем процесс
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        std::cout << "[!] Не удалось открыть процесс! Запусти от администратора." << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "2. [✓] Процесс открыт" << std::endl;

    // 4. Получаем базу client.dll
    uintptr_t clientBase = GetModuleBaseAddress(pid, L"client.dll");
    if (clientBase == 0) {
        std::cout << "[!] client.dll не найдена!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "3. [✓] client.dll: 0x" << std::hex << clientBase << std::dec << std::endl;

    // 5. Получаем размер экрана
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // 6. Создаем оверлей
    g_hOverlay = CreateOverlay(screenWidth, screenHeight);
    if (!g_hOverlay) {
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "4. [✓] Оверлей создан" << std::endl;

    std::cout << std::endl << "ESP запущен! Нажми ESC для выхода..." << std::endl;
    std::cout << "Для теста: запусти матч с ботами!" << std::endl;

    // 7. Основной цикл
    MSG msg = {};
    int frameCount = 0;
    
    while (g_running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
        }
        
        if (!g_running) break;
        
        UpdateOverlayPosition(g_hOverlay);
        
        ViewMatrix vm = GetViewMatrixFromMemory(hProcess, clientBase, localOffsets.dwViewMatrix);
        
        // Читаем локального игрока
        uintptr_t localPlayerPawn = 0;
        if (!ReadMemory(hProcess, clientBase + localOffsets.dwLocalPlayerPawn, localPlayerPawn)) {
            Sleep(16);
            continue;
        }
        
        if (!IsValidAddress(localPlayerPawn)) {
            Sleep(16);
            continue;
        }
        
        PlayerInfo localPlayer = {};
        ReadMemory(hProcess, localPlayerPawn + localOffsets.m_iHealth, localPlayer.health);
        
        if (localOffsets.m_vecOrigin == 0x80) {
            uintptr_t sceneNode = 0;
            ReadMemory(hProcess, localPlayerPawn + 0x330, sceneNode);
            if (IsValidAddress(sceneNode)) {
                ReadMemory(hProcess, sceneNode + 0x80, localPlayer.position);
            }
        } else {
            ReadMemory(hProcess, localPlayerPawn + localOffsets.m_vecOrigin, localPlayer.position);
        }
        localPlayer.isAlive = (localPlayer.health > 0);
        
        // Читаем команду локального игрока через Controller
        localPlayer.team = 0;
        if (localOffsets.dwLocalPlayerController != 0) {
            uintptr_t controller = 0;
            if (ReadMemory(hProcess, clientBase + localOffsets.dwLocalPlayerController, controller)) {
                if (IsValidAddress(controller)) {
                    ReadMemory(hProcess, controller + localOffsets.m_iTeamNum, localPlayer.team);
                }
            }
        }
        
        // Получаем игроков
        auto players = GetPlayers(hProcess, clientBase, localOffsets, localPlayer);
        
        if (frameCount++ % 60 == 0) {
            std::cout << "[DEBUG] Players found: " << players.size() 
                      << " | Local team: " << localPlayer.team << std::endl;
        }
        
        HDC hdc = GetDC(g_hOverlay);
        
        RECT rect;
        GetClientRect(g_hOverlay, &rect);
        HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rect, clearBrush);
        DeleteObject(clearBrush);
        
        DrawESP(hdc, players, localPlayer, vm, screenWidth, screenHeight);
        
        ReleaseDC(g_hOverlay, hdc);
        
        Sleep(16);
    }
    
    if (g_hOverlay) {
        DestroyWindow(g_hOverlay);
        g_hOverlay = NULL;
    }
    
    CloseHandle(hProcess);
    std::cout << "ESP остановлен." << std::endl;
    return 0;
}