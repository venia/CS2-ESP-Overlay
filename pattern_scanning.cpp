#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <cstdint>

// ============================================
// СТРУКТУРЫ
// ============================================

struct Offsets {
    uintptr_t dwEntityList;
    uintptr_t dwLocalPlayerPawn;
    uintptr_t dwLocalPlayerController;
    uintptr_t dwViewMatrix;
    uintptr_t dwGameEntitySystem;
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
// PATTERN SCANNING
// ============================================

class PatternScanner {
public:
    // Ищет сигнатуру в памяти процесса
    static uintptr_t FindPattern(HANDLE hProcess, uintptr_t startAddress, size_t size, 
                                 const std::vector<BYTE>& pattern, const std::string& mask) {
        std::vector<BYTE> buffer(size);
        SIZE_T bytesRead;
        
        if (!ReadProcessMemory(hProcess, (LPCVOID)startAddress, buffer.data(), size, &bytesRead)) {
            return 0;
        }
        
        for (size_t i = 0; i <= bytesRead - pattern.size(); i++) {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); j++) {
                if (mask[j] == 'x' && pattern[j] != buffer[i + j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return startAddress + i;
            }
        }
        
        return 0;
    }
    
    // Ищет сигнатуру во всей памяти модуля
    static uintptr_t FindPatternInModule(HANDLE hProcess, uintptr_t moduleBase, size_t moduleSize,
                                         const std::vector<BYTE>& pattern, const std::string& mask) {
        // Разбиваем на куски по 1MB для оптимизации
        const size_t CHUNK_SIZE = 0x100000;
        
        for (size_t offset = 0; offset < moduleSize; offset += CHUNK_SIZE) {
            size_t currentSize = std::min(CHUNK_SIZE, moduleSize - offset);
            uintptr_t result = FindPattern(hProcess, moduleBase + offset, currentSize, pattern, mask);
            if (result != 0) {
                return result;
            }
        }
        
        return 0;
    }
};

// ============================================
// ПОЛУЧЕНИЕ СМЕЩЕНИЙ ЧЕРЕЗ PATTERN SCANNING
// ============================================

Offsets GetOffsetsFromMemory(HANDLE hProcess, uintptr_t clientBase) {
    Offsets offsets = {};
    SIZE_T bytesRead;
    
    // 1. Находим dwLocalPlayerPawn
    // Сигнатура: "48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 50 ?"
    std::vector<BYTE> localPlayerPattern = {
        0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x00
    };
    std::string localPlayerMask = "xxx????xxx?xxxxxx?";
    
    // 2. Находим dwViewMatrix
    std::vector<BYTE> viewMatrixPattern = {
        0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x00, 0xF3, 0x0F, 0x10, 0x01
    };
    std::string viewMatrixMask = "xxx????xxx?xxxx";
    
    // 3. Находим m_iHealth в C_BaseEntity
    // Обычно 0x34C, но проверим через чтение
    offsets.m_iHealth = 0x34C;
    offsets.m_iTeamNum = 0x3E7;
    offsets.m_vecOrigin = 0x80;
    
    // 4. Пробуем найти dwEntityList через старый метод
    uintptr_t entityList = 0;
    std::vector<BYTE> entityListPattern = {
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x00
    };
    std::string entityListMask = "xxx????xxx?x";
    
    // 5. Получаем смещения из паттернов
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t currentAddress = clientBase;
    size_t clientSize = 0;
    
    // Получаем размер client.dll
    if (VirtualQueryEx(hProcess, (LPCVOID)clientBase, &mbi, sizeof(mbi))) {
        clientSize = mbi.RegionSize;
    }
    
    std::cout << "   Поиск смещений через паттерны..." << std::endl;
    
    // Ищем LocalPlayer
    uintptr_t localPlayerAddr = PatternScanner::FindPatternInModule(
        hProcess, clientBase, clientSize, localPlayerPattern, localPlayerMask);
    
    if (localPlayerAddr != 0) {
        // Читаем смещение из инструкции mov rcx, [rip+offset]
        int32_t offset = 0;
        ReadProcessMemory(hProcess, (LPCVOID)(localPlayerAddr + 3), &offset, sizeof(offset), &bytesRead);
        offsets.dwLocalPlayerPawn = (uintptr_t)(localPlayerAddr + 7 + offset) - clientBase;
        std::cout << "   dwLocalPlayerPawn: 0x" << std::hex << offsets.dwLocalPlayerPawn << std::dec << std::endl;
    }
    
    // Ищем ViewMatrix
    uintptr_t viewMatrixAddr = PatternScanner::FindPatternInModule(
        hProcess, clientBase, clientSize, viewMatrixPattern, viewMatrixMask);
    
    if (viewMatrixAddr != 0) {
        int32_t offset = 0;
        ReadProcessMemory(hProcess, (LPCVOID)(viewMatrixAddr + 3), &offset, sizeof(offset), &bytesRead);
        offsets.dwViewMatrix = (uintptr_t)(viewMatrixAddr + 7 + offset) - clientBase;
        std::cout << "   dwViewMatrix: 0x" << std::hex << offsets.dwViewMatrix << std::dec << std::endl;
    }
    
    // Ищем EntityList
    uintptr_t entityListAddr = PatternScanner::FindPatternInModule(
        hProcess, clientBase, clientSize, entityListPattern, entityListMask);
    
    if (entityListAddr != 0) {
        int32_t offset = 0;
        ReadProcessMemory(hProcess, (LPCVOID)(entityListAddr + 3), &offset, sizeof(offset), &bytesRead);
        offsets.dwEntityList = (uintptr_t)(entityListAddr + 7 + offset) - clientBase;
        std::cout << "   dwEntityList: 0x" << std::hex << offsets.dwEntityList << std::dec << std::endl;
        offsets.dwGameEntitySystem = offsets.dwEntityList;
    }
    
    // Ищем LocalPlayerController
    std::vector<BYTE> controllerPattern = {
        0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x00, 0x48, 0x8B, 0x41, 0x00
    };
    std::string controllerMask = "xxx????xxx?xxx?";
    
    uintptr_t controllerAddr = PatternScanner::FindPatternInModule(
        hProcess, clientBase, clientSize, controllerPattern, controllerMask);
    
    if (controllerAddr != 0) {
        int32_t offset = 0;
        ReadProcessMemory(hProcess, (LPCVOID)(controllerAddr + 3), &offset, sizeof(offset), &bytesRead);
        offsets.dwLocalPlayerController = (uintptr_t)(controllerAddr + 7 + offset) - clientBase;
        std::cout << "   dwLocalPlayerController: 0x" << std::hex << offsets.dwLocalPlayerController << std::dec << std::endl;
    }
    
    // Если не нашли через паттерны, пробуем стандартные смещения
    if (offsets.dwLocalPlayerPawn == 0) {
        offsets.dwLocalPlayerPawn = 0x23C6268;
        std::cout << "   [!] Использую запасное смещение dwLocalPlayerPawn: 0x" << std::hex << offsets.dwLocalPlayerPawn << std::dec << std::endl;
    }
    
    if (offsets.dwViewMatrix == 0) {
        offsets.dwViewMatrix = 0x23CB830;
        std::cout << "   [!] Использую запасное смещение dwViewMatrix: 0x" << std::hex << offsets.dwViewMatrix << std::dec << std::endl;
    }
    
    if (offsets.dwEntityList == 0) {
        offsets.dwEntityList = 0x2571220;
        offsets.dwGameEntitySystem = offsets.dwEntityList;
        std::cout << "   [!] Использую запасное смещение dwEntityList: 0x" << std::hex << offsets.dwEntityList << std::dec << std::endl;
    }
    
    return offsets;
}

// ============================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
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
// ФУНКЦИИ РИСОВАНИЯ (сокращенно для демонстрации)
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

// ============================================
// ГЛАВНАЯ ПРОГРАММА
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=== CS2 ESP Overlay (Pattern Scanning) ===" << std::endl << std::endl;
    
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

    // 4. Получаем смещения через Pattern Scanning
    Offsets offsets = GetOffsetsFromMemory(hProcess, clientBase);
    
    std::cout << std::endl << "   [✓] Итоговые смещения:" << std::endl;
    std::cout << "   dwEntityList: 0x" << std::hex << offsets.dwEntityList << std::dec << std::endl;
    std::cout << "   dwLocalPlayerPawn: 0x" << std::hex << offsets.dwLocalPlayerPawn << std::dec << std::endl;
    std::cout << "   dwLocalPlayerController: 0x" << std::hex << offsets.dwLocalPlayerController << std::dec << std::endl;
    std::cout << "   dwViewMatrix: 0x" << std::hex << offsets.dwViewMatrix << std::dec << std::endl;
    std::cout << "   m_iHealth: 0x" << std::hex << offsets.m_iHealth << std::dec << std::endl;
    std::cout << "   m_iTeamNum: 0x" << std::hex << offsets.m_iTeamNum << std::dec << std::endl;
    std::cout << "   m_vecOrigin: 0x" << std::hex << offsets.m_vecOrigin << std::dec << std::endl;

    CloseHandle(hProcess);
    std::cout << std::endl << "Нажми Enter для выхода...";
    std::cin.get();
    return 0;
}