// cs_main.cpp
// Задача: показать ESP, записывать данные в secret/ccs.trs, читать secret/poc.trs

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

// ============================================
// СТРУКТУРЫ
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
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================

HWND g_hOverlay = NULL;
bool g_running = true;
Offsets g_offsets = {};
bool g_offsetsFound = false;
int g_searchAttempts = 0;

// ============================================
// БЕЗОПАСНАЯ РАБОТА С ФАЙЛАМИ
// ============================================

bool WriteJsonToFile(const std::string& filename, const std::string& data) {
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

std::string ReadJsonFromFile(const std::string& filename) {
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
// ЗАПУСК pattern_scanning.exe
// ============================================

bool StartPatternScanner() {
    std::string cmd = "start /B pattern_scanning.exe";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    
    if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::cout << "[✓] pattern_scanning.exe запущен" << std::endl;
        return true;
    }
    std::cout << "[!] Не удалось запустить pattern_scanning.exe!" << std::endl;
    return false;
}

// ============================================
// ЧТЕНИЕ СМЕЩЕНИЙ ИЗ poc.trs
// ============================================

bool ReadOffsetsFromFile(Offsets& offsets) {
    std::string jsonData = ReadJsonFromFile("secret/poc.trs");
    if (jsonData.empty()) return false;
    
    // Упрощенный парсинг JSON (без библиотеки)
    // В реальном коде используй jsoncpp или nlohmann/json
    try {
        // Ищем status
        size_t statusPos = jsonData.find("\"status\":");
        if (statusPos != std::string::npos) {
            size_t endPos = jsonData.find(",", statusPos);
            std::string status = jsonData.substr(statusPos + 10, endPos - statusPos - 11);
            if (status != "found") return false;
        }
        
        // Ищем offsets
        size_t offsetsPos = jsonData.find("\"offsets\":");
        if (offsetsPos != std::string::npos) {
            // Ищем каждое смещение
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
// ПОЛУЧЕНИЕ ИГРОКОВ
// ============================================

std::vector<PlayerInfo> GetPlayers(HANDLE hProcess, uintptr_t clientBase, Offsets offsets, PlayerInfo& localPlayer) {
    std::vector<PlayerInfo> players;
    
    // Используем старый метод через entityList
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
             ViewMatrix vm, int screenWidth, int screenHeight, bool offsetsFound, int attempts) {
    
    // ============================================
    // ОТЛАДОЧНАЯ ИНФОРМАЦИЯ
    // ============================================
    
    SetTextColor(hdc, RGB(0, 255, 0));
    SetBkMode(hdc, TRANSPARENT);
    
    HFONT debugFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    
    HFONT oldFont = (HFONT)SelectObject(hdc, debugFont);
    
    // Тень
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, 9, 9, "ESP ACTIVE", 10);
    TextOutA(hdc, 11, 9, "ESP ACTIVE", 10);
    TextOutA(hdc, 10, 8, "ESP ACTIVE", 10);
    TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
    
    if (offsetsFound) {
        SetTextColor(hdc, RGB(0, 255, 0));
        TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
        
        char foundText[64];
        sprintf(foundText, "✅ Параметры найдены! (%d попыток)", attempts);
        SetTextColor(hdc, RGB(0, 255, 0));
        TextOutA(hdc, 10, 35, foundText, (int)strlen(foundText));
    } else {
        SetTextColor(hdc, RGB(255, 255, 0));
        TextOutA(hdc, 10, 10, "ESP ACTIVE", 10);
        
        char searchingText[64];
        sprintf(searchingText, "🔍 Поиск смещений... (попыток: %d)", attempts);
        SetTextColor(hdc, RGB(255, 255, 0));
        TextOutA(hdc, 10, 35, searchingText, (int)strlen(searchingText));
    }
    
    char infoText[256];
    sprintf(infoText, "Players: %d | HP: %d | Team: %d", 
            (int)players.size(), localPlayer.health, localPlayer.team);
    
    SetTextColor(hdc, RGB(0, 255, 255));
    TextOutA(hdc, 10, 60, infoText, (int)strlen(infoText));
    
    char posText[256];
    sprintf(posText, "Pos: (%.1f, %.1f, %.1f)", 
            localPlayer.position.x, localPlayer.position.y, localPlayer.position.z);
    
    SetTextColor(hdc, RGB(255, 255, 0));
    TextOutA(hdc, 10, 85, posText, (int)strlen(posText));
    
    SelectObject(hdc, oldFont);
    DeleteObject(debugFont);
    
    // ============================================
    // РИСУЕМ ИГРОКОВ (только если есть смещения)
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
// ViewMatrix
// ============================================

ViewMatrix GetViewMatrixFromMemory(HANDLE hProcess, uintptr_t clientBase, uintptr_t viewMatrixOffset) {
    ViewMatrix vm = {};
    ReadMemory(hProcess, clientBase + viewMatrixOffset, vm);
    return vm;
}

// ============================================
// ОСНОВНАЯ ПРОГРАММА
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=== CS2 ESP Overlay (Auto-Scanner) ===" << std::endl << std::endl;
    
    // 1. Создаем папку secret и очищаем файлы
    CreateDirectoryIfNotExists("secret");
    DeleteFileIfExists("secret/ccs.trs");
    DeleteFileIfExists("secret/poc.trs");
    
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

    // 5. Запускаем pattern_scanning.exe
    if (!StartPatternScanner()) {
        std::cout << "[!] Продолжаем без сканера (используем запасные смещения)" << std::endl;
    }

    // 6. Создаем оверлей
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    g_hOverlay = CreateOverlay(screenWidth, screenHeight);
    if (!g_hOverlay) {
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "4. [✓] Оверлей создан" << std::endl;

    std::cout << std::endl << "ESP запущен! Нажми ESC для выхода..." << std::endl;
    std::cout << "Начни игру и получай урон для поиска смещений!" << std::endl << std::endl;

    // 7. Основной цикл
    MSG msg = {};
    int frameCount = 0;
    auto lastWriteTime = std::chrono::steady_clock::now();
    
    // Запасные смещения (если сканер не сработает)
    Offsets fallbackOffsets = {};
    fallbackOffsets.dwEntityList = 0x2571220;
    fallbackOffsets.dwLocalPlayerPawn = 0x23C6268;
    fallbackOffsets.dwLocalPlayerController = 0x23A0F30;
    fallbackOffsets.dwViewMatrix = 0x23CB830;
    fallbackOffsets.m_iHealth = 0x34C;
    fallbackOffsets.m_iTeamNum = 0x3E7;
    fallbackOffsets.m_vecOrigin = 0x80;
    
    g_offsets = fallbackOffsets;
    
    while (g_running) {
        // Обработка сообщений Windows
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
        }
        
        if (!g_running) break;
        
        // Обновляем позицию оверлея
        UpdateOverlayPosition(g_hOverlay);
        
        // Читаем локального игрока (даже без смещений, чтобы показывать статус)
        uintptr_t localPlayerPawn = 0;
        int localHealth = 0;
        int localTeam = 0;
        Vector3 localPos = {0, 0, 0};
        
        if (g_offsetsFound) {
            // Используем найденные смещения
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
            // Пытаемся прочитать через запасные смещения
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
        
        // Читаем игроков (только если есть смещения)
        std::vector<PlayerInfo> players;
        ViewMatrix vm = {};
        
        if (g_offsetsFound) {
            vm = GetViewMatrixFromMemory(hProcess, clientBase, g_offsets.dwViewMatrix);
            players = GetPlayers(hProcess, clientBase, g_offsets, localPlayer);
        } else {
            // Пробуем через запасные
            vm = GetViewMatrixFromMemory(hProcess, clientBase, fallbackOffsets.dwViewMatrix);
            players = GetPlayers(hProcess, clientBase, fallbackOffsets, localPlayer);
        }
        
        // ============================================
        // ЗАПИСЬ В secret/ccs.trs (каждую секунду)
        // ============================================
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWriteTime).count() >= 1000) {
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
            
            WriteJsonToFile("secret/ccs.trs", json.str());
            
            g_searchAttempts++;
        }
        
        // ============================================
        // ЧТЕНИЕ ИЗ secret/poc.trs (проверка, не найдены ли смещения)
        // ============================================
        if (!g_offsetsFound) {
            Offsets newOffsets = {};
            if (ReadOffsetsFromFile(newOffsets)) {
                if (newOffsets.m_iHealth != 0) {
                    g_offsets = newOffsets;
                    g_offsetsFound = true;
                    std::cout << "[✓] Смещения получены из secret/poc.trs!" << std::endl;
                    std::cout << "   m_iHealth: 0x" << std::hex << g_offsets.m_iHealth << std::dec << std::endl;
                }
            }
        }
        
        // ============================================
        // РИСУЕМ ESP
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
    // ЗАКРЫТИЕ
    // ============================================
    
    // Удаляем только ccs.trs, poc.trs оставляем для дебага
    DeleteFileIfExists("secret/ccs.trs");
    
    if (g_hOverlay) {
        DestroyWindow(g_hOverlay);
        g_hOverlay = NULL;
    }
    
    CloseHandle(hProcess);
    std::cout << "ESP остановлен." << std::endl;
    return 0;
}