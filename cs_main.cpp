#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

// Из offsets.hpp
namespace client_dll {
    constexpr std::ptrdiff_t dwEntityList = 0x2572230;
    constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x23C7268;
    constexpr std::ptrdiff_t dwViewMatrix = 0x23CC830;
}

// Из client_dll.hpp - ПОДСТАВЬ ПРАВИЛЬНЫЕ ЗНАЧЕНИЯ!
namespace C_BaseEntity {
    constexpr std::ptrdiff_t m_iHealth = 0x34C;
    constexpr std::ptrdiff_t m_iTeamNum = 0x3E7;
    constexpr std::ptrdiff_t m_lifeState = 0x354;
    constexpr std::ptrdiff_t m_fFlags = 0x3F4;
    constexpr std::ptrdiff_t m_vecOrigin = 0x80;  // ← НАЙДИ В ФАЙЛЕ!
    constexpr std::ptrdiff_t m_vecVelocity = 0x430;
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
    if (!ReadMemory(hProcess, clientBase + client_dll::dwLocalPlayerPawn, localPlayerPawn)) {
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
    
    ReadMemory(hProcess, localPlayerPawn + C_BaseEntity::m_iHealth, localHealth);
    ReadMemory(hProcess, localPlayerPawn + C_BaseEntity::m_iTeamNum, localTeam);
    ReadMemory(hProcess, localPlayerPawn + C_BaseEntity::m_lifeState, localLifeState);
    ReadMemory(hProcess, localPlayerPawn + C_BaseEntity::m_fFlags, localFlags);
    ReadMemory(hProcess, localPlayerPawn + C_BaseEntity::m_vecOrigin, localPos);
    
    std::cout << "   [Локальный] HP: " << localHealth 
              << " | Команда: " << localTeam 
              << " | LifeState: " << localLifeState
              << " | Flags: 0x" << std::hex << localFlags << std::dec
              << std::endl;
    std::cout << "   [Локальный] Позиция: (" << localPos.x << ", " << localPos.y << ", " << localPos.z << ")" << std::endl;

    // 6. Читаем список сущностей
    uintptr_t entityList = 0;
    if (!ReadMemory(hProcess, clientBase + client_dll::dwEntityList, entityList)) {
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
        if (!ReadMemory(hProcess, playerPawn + C_BaseEntity::m_iTeamNum, team)) {
            continue;
        }
        
        // Игнорируем неигровые сущности
        if (team != 2 && team != 3) {
            continue;
        }

        // Читаем здоровье
        int health = 0;
        ReadMemory(hProcess, playerPawn + C_BaseEntity::m_iHealth, health);
        
        // Игнорируем мертвых
        if (health <= 0 || health > 100) {
            continue;
        }

        // Читаем позицию
        Vector3 pos = {0, 0, 0};
        ReadMemory(hProcess, playerPawn + C_BaseEntity::m_vecOrigin, pos);
        
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