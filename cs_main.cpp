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

struct Offsets {
    uintptr_t dwEntityList;
    uintptr_t dwLocalPlayerPawn;
    uintptr_t dwViewMatrix;
    uintptr_t m_iHealth;
    uintptr_t m_iTeamNum;
    uintptr_t m_lifeState;
    uintptr_t m_fFlags;
    uintptr_t m_vecOrigin;
};

// ============================================
// БАЗОВЫЕ ФУНКЦИИ
// ============================================

// Запускает cs2-dumper и ждет завершения
bool RunCS2Dumper() {
    std::string cmd = "cs2-dumper.exe --output ./output";
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    
    if (CreateProcess(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

// Читает offsets.hpp и извлекает смещения
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
                else if (name == "dwViewMatrix") offsets.dwViewMatrix = addr;
            }
        }
    }
    
    return offsets;
}

// Читает client_dll.hpp и извлекает смещения для C_BaseEntity
Offsets ParseClientDll(const std::string& filepath) {
    Offsets offsets = {};
    std::ifstream file(filepath);
    std::string line;
    std::regex pattern(R"(constexpr std::ptrdiff_t (\w+) = (0x[0-9A-Fa-f]+);)");
    
    bool inBaseEntity = false;
    
    while (std::getline(file, line)) {
        if (line.find("namespace C_BaseEntity {") != std::string::npos) {
            inBaseEntity = true;
            continue;
        }
        if (line.find("}") != std::string::npos && inBaseEntity) {
            inBaseEntity = false;
            continue;
        }
        
        if (inBaseEntity) {
            std::smatch match;
            if (std::regex_search(line, match, pattern)) {
                std::string name = match[1].str();
                std::string value = match[2].str();
                uintptr_t addr = std::stoull(value, nullptr, 16);
                
                if (name == "m_iHealth") offsets.m_iHealth = addr;
                else if (name == "m_iTeamNum") offsets.m_iTeamNum = addr;
                else if (name == "m_lifeState") offsets.m_lifeState = addr;
                else if (name == "m_fFlags") offsets.m_fFlags = addr;
                else if (name == "m_vecOrigin") offsets.m_vecOrigin = addr;
            }
        }
    }
    
    return offsets;
}

// Главная функция для получения смещений
Offsets GetOffsets() {
    // 1. Запускаем cs2-dumper
    std::cout << "Запуск cs2-dumper для получения актуальных смещений..." << std::endl;
    
    // Создаем папку для вывода
    CreateDirectory("./output", NULL);
    
    // Запускаем dumper
    if (!RunCS2Dumper()) {
        std::cout << "   [!] Не удалось запустить cs2-dumper!" << std::endl;
        return {};
    }
    
    // 2. Читаем offsets.hpp
    Offsets offsets = ParseOffsets("./output/offsets.hpp");
    if (offsets.dwEntityList == 0) {
        std::cout << "   [!] Не удалось прочитать offsets.hpp!" << std::endl;
        return {};
    }
    
    // 3. Читаем client_dll.hpp
    Offsets entityOffsets = ParseClientDll("./output/client_dll.hpp");
    
    // 4. Объединяем
    offsets.m_iHealth = entityOffsets.m_iHealth;
    offsets.m_iTeamNum = entityOffsets.m_iTeamNum;
    offsets.m_lifeState = entityOffsets.m_lifeState;
    offsets.m_fFlags = entityOffsets.m_fFlags;
    offsets.m_vecOrigin = entityOffsets.m_vecOrigin;
    
    std::cout << "   [✓] Смещения получены!" << std::endl;
    std::cout << "   dwEntityList: 0x" << std::hex << offsets.dwEntityList << std::dec << std::endl;
    std::cout << "   dwLocalPlayerPawn: 0x" << std::hex << offsets.dwLocalPlayerPawn << std::dec << std::endl;
    std::cout << "   dwViewMatrix: 0x" << std::hex << offsets.dwViewMatrix << std::dec << std::endl;
    std::cout << "   m_iHealth: 0x" << std::hex << offsets.m_iHealth << std::dec << std::endl;
    std::cout << "   m_iTeamNum: 0x" << std::hex << offsets.m_iTeamNum << std::dec << std::endl;
    std::cout << "   m_vecOrigin: 0x" << std::hex << offsets.m_vecOrigin << std::dec << std::endl;
    
    return offsets;
}

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

struct Vector3 {
    float x, y, z;
};

bool IsValidAddress(uintptr_t address) {
    return address > 0x10000 && address < 0x7FFFFFFF0000;
}

// ============================================
// ОСНОВНАЯ ПРОГРАММА
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=== CS2 GetOffsets ===" << std::endl << std::endl;
    Offsets localOffsets = GetOffsets();

    std::cout << "=== CS2 Memory Reader ===" << std::endl << std::endl;

    // 1. Находим CS2
    DWORD pid = GetProcessIdByName(L"cs2.exe");
    if (pid == 0) {
        std::cout << "[!] CS2 не запущена!" << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "1. [✓] CS2 PID: " << pid << std::endl;

    // 2. Открываем процесс
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        std::cout << "[!] Не удалось открыть процесс! Запусти от администратора." << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "2. [✓] Процесс открыт" << std::endl;

    // 3. Получаем базу client.dll
    uintptr_t clientBase = GetModuleBaseAddress(pid, L"client.dll");
    if (clientBase == 0) {
        std::cout << "[!] client.dll не найдена!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "3. [✓] client.dll: 0x" << std::hex << clientBase << std::dec << std::endl;

    // 4. Читаем локального игрока
    uintptr_t localPlayerPawn = 0;
    if (!ReadMemory(hProcess, clientBase + localOffsets.dwLocalPlayerPawn, localPlayerPawn)) {
        std::cout << "[!] Не удалось прочитать локального игрока!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    
    if (!IsValidAddress(localPlayerPawn)) {
        std::cout << "[!] Некорректный адрес локального игрока!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    
    std::cout << "4. [✓] Локальный игрок: 0x" << std::hex << localPlayerPawn << std::dec << std::endl;

    // 5. Читаем данные локального игрока
    int localHealth = 0;
    int localTeam = 0;
    int localLifeState = 0;
    int localFlags = 0;
    Vector3 localPos = {0, 0, 0};
    
    ReadMemory(hProcess, localPlayerPawn + localOffsets.m_iHealth, localHealth);
    ReadMemory(hProcess, localPlayerPawn + localOffsets.m_iTeamNum, localTeam);
    ReadMemory(hProcess, localPlayerPawn + localOffsets.m_lifeState, localLifeState);
    ReadMemory(hProcess, localPlayerPawn + localOffsets.m_fFlags, localFlags);
    ReadMemory(hProcess, localPlayerPawn + localOffsets.m_vecOrigin, localPos);
    
    std::cout << "   [Локальный] HP: " << localHealth 
              << " | Команда: " << localTeam 
              << " | LifeState: " << localLifeState
              << " | Flags: 0x" << std::hex << localFlags << std::dec
              << std::endl;
    std::cout << "   [Локальный] Позиция: (" << localPos.x << ", " << localPos.y << ", " << localPos.z << ")" << std::endl;

    // 6. Читаем список сущностей
    uintptr_t entityList = 0;
    if (!ReadMemory(hProcess, clientBase + localOffsets.dwEntityList, entityList)) {
        std::cout << "[!] Не удалось прочитать список сущностей!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "5. [✓] Список сущностей: 0x" << std::hex << entityList << std::dec << std::endl;

    // 7. Проходим по игрокам
    std::cout << std::endl << "6. Игроки на сервере:" << std::endl;
    
    int playerCount = 0;
    for (int i = 0; i < 64; i++) {
        uintptr_t playerPawn = 0;
        uintptr_t entityEntry = entityList + (i + 1) * 0x10;
        
        if (!ReadMemory(hProcess, entityEntry, playerPawn) || !IsValidAddress(playerPawn)) {
            continue;
        }

        // Пропускаем себя
        if (playerPawn == localPlayerPawn) {
            continue;
        }

        // Читаем команду
        int team = 0;
        if (!ReadMemory(hProcess, playerPawn + localOffsets.m_iTeamNum, team)) {
            continue;
        }
        
        // Игнорируем неигровые сущности
        if (team != 2 && team != 3) {
            continue;
        }

        // Читаем здоровье
        int health = 0;
        ReadMemory(hProcess, playerPawn + localOffsets.m_iHealth, health);
        
        // Игнорируем мертвых
        if (health <= 0 || health > 100) {
            continue;
        }

        // Читаем позицию
        Vector3 pos = {0, 0, 0};
        ReadMemory(hProcess, playerPawn + localOffsets.m_vecOrigin, pos);
        
        bool isEnemy = (team != localTeam);
        playerCount++;
        
        std::cout << "   [" << i << "] Команда: " << team 
                  << " | HP: " << health 
                  << " | Позиция: (" << pos.x << ", " << pos.y << ", " << pos.z << ")"
                  << (isEnemy ? " [ВРАГ!]" : " [Союзник]")
                  << std::endl;
    }

    if (playerCount == 0) {
        std::cout << "   Нет живых игроков на сервере" << std::endl;
        std::cout << "   (Ты один на сервере или в тренировочном режиме)" << std::endl;
    }

    CloseHandle(hProcess);
    std::cout << std::endl << "Нажми Enter для выхода...";
    std::cin.get();
    return 0;
}