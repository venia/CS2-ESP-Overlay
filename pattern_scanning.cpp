// pattern_scanning.cpp
// Задача: найти смещения через сканирование памяти и сохранить в secret/poc.trs

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#include <json-c/json.h>  // Требуется установить: pacman -S mingw-w64-ucrt-x86_64-json-c

// ============================================
// СТРУКТУРЫ
// ============================================

struct Vector3 {
    float x, y, z;
};

struct Offsets {
    uintptr_t dwEntityList;
    uintptr_t dwLocalPlayerPawn;
    uintptr_t dwLocalPlayerController;
    uintptr_t dwViewMatrix;
    uintptr_t m_iHealth;
    uintptr_t m_iTeamNum;
    uintptr_t m_vecOrigin;
};

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
// БЕЗОПАСНАЯ РАБОТА С ФАЙЛАМИ
// ============================================

bool WriteJsonToFile(const std::string& filename, const std::string& data) {
    std::string tempFile = filename + ".tmp";
    std::ofstream out(tempFile);
    if (!out.is_open()) return false;
    out << data;
    out.close();
    
    // Атомарное переименование
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

// ============================================
// СКАНИРОВАНИЕ ПАМЯТИ (ПОИСК ЗДОРОВЬЯ)
// ============================================

struct Candidate {
    uintptr_t address;
    int currentValue;
    int previousValue;
    int stableCount;
};

std::vector<Candidate> candidates;
int totalAttempts = 0;

// Первый проход: найти ВСЕ адреса с текущим здоровьем
std::vector<uintptr_t> FindCandidates(HANDLE hProcess, uintptr_t startAddress, size_t size, int targetValue) {
    std::vector<uintptr_t> result;
    std::vector<BYTE> buffer(size);
    SIZE_T bytesRead;
    
    if (!ReadProcessMemory(hProcess, (LPCVOID)startAddress, buffer.data(), size, &bytesRead)) {
        return result;
    }
    
    for (size_t i = 0; i <= bytesRead - sizeof(int); i += sizeof(int)) {
        int value = *(int*)(buffer.data() + i);
        if (value == targetValue) {
            result.push_back(startAddress + i);
        }
    }
    
    return result;
}

// Фильтрация кандидатов: оставить только те, где значение изменилось
std::vector<Candidate> FilterCandidates(HANDLE hProcess, const std::vector<Candidate>& oldCandidates, int newValue) {
    std::vector<Candidate> result;
    
    for (const auto& cand : oldCandidates) {
        int currentValue = 0;
        if (ReadMemory(hProcess, cand.address, currentValue)) {
            if (currentValue == newValue && currentValue != cand.previousValue) {
                Candidate newCand = cand;
                newCand.currentValue = currentValue;
                newCand.previousValue = cand.currentValue;
                newCand.stableCount = 0;
                result.push_back(newCand);
            } else if (currentValue == newValue && currentValue == cand.previousValue) {
                // Значение стабильно
                Candidate newCand = cand;
                newCand.currentValue = currentValue;
                newCand.previousValue = cand.currentValue;
                newCand.stableCount = cand.stableCount + 1;
                result.push_back(newCand);
            }
        }
    }
    
    return result;
}

// ============================================
// ОСНОВНАЯ ЛОГИКА pattern_scanning.exe
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    std::cout << "=== Pattern Scanner (Runtime Scanning) ===" << std::endl << std::endl;
    
    // 1. Создаем папку secret
    CreateDirectoryIfNotExists("secret");
    
    // 2. Подключаемся к CS2
    DWORD pid = GetProcessIdByName(L"cs2.exe");
    if (pid == 0) {
        std::cout << "[!] CS2 не запущена!" << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "1. [✓] CS2 PID: " << pid << std::endl;
    
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ, FALSE, pid);
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
    
    // 4. Получаем размер client.dll
    MEMORY_BASIC_INFORMATION mbi;
    size_t clientSize = 0;
    if (VirtualQueryEx(hProcess, (LPCVOID)clientBase, &mbi, sizeof(mbi))) {
        clientSize = mbi.RegionSize;
    }
    std::cout << "4. [✓] Размер client.dll: 0x" << std::hex << clientSize << std::dec << " байт" << std::endl;
    
    std::cout << std::endl << "Ожидание данных от cs_main.exe..." << std::endl;
    std::cout << "Начни игру и получай урон для поиска!" << std::endl << std::endl;
    
    // 5. Основной цикл сканирования
    bool found = false;
    Offsets foundOffsets = {};
    int lastHealth = -1;
    int stableCount = 0;
    
    while (!found) {
        // Читаем данные из secret/ccs.trs
        std::string jsonData = ReadJsonFromFile("secret/ccs.trs");
        if (jsonData.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Парсим JSON (упрощенно, без библиотеки)
        // В реальном коде используй jsoncpp или nlohmann/json
        int currentHealth = 0;
        size_t pos = jsonData.find("\"health\":");
        if (pos != std::string::npos) {
            currentHealth = std::stoi(jsonData.substr(pos + 9));
        }
        
        if (currentHealth == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Если здоровье изменилось
        if (currentHealth != lastHealth && lastHealth != -1) {
            totalAttempts++;
            
            std::cout << "[*] Попытка #" << totalAttempts << " | Здоровье: " << lastHealth << " → " << currentHealth << std::endl;
            
            if (candidates.empty()) {
                // Первый поиск: находим все адреса с текущим здоровьем
                std::cout << "[*] Первый поиск: сканируем " << (clientSize / 1024 / 1024) << " MB памяти..." << std::endl;
                
                auto addresses = FindCandidates(hProcess, clientBase, clientSize, currentHealth);
                
                for (uintptr_t addr : addresses) {
                    Candidate cand;
                    cand.address = addr;
                    cand.currentValue = currentHealth;
                    cand.previousValue = lastHealth;
                    cand.stableCount = 0;
                    candidates.push_back(cand);
                }
                
                std::cout << "[*] Найдено " << candidates.size() << " кандидатов" << std::endl;
            } else {
                // Фильтруем кандидатов
                auto filtered = FilterCandidates(hProcess, candidates, currentHealth);
                candidates = filtered;
                
                std::cout << "[*] Осталось " << candidates.size() << " кандидатов" << std::endl;
                
                // Если осталось 1 кандидат - он найден!
                if (candidates.size() == 1) {
                    uintptr_t healthAddress = candidates[0].address;
                    uintptr_t healthOffset = healthAddress - clientBase;
                    
                    std::cout << "[✓] Здоровье найдено!" << std::endl;
                    std::cout << "    Адрес: 0x" << std::hex << healthAddress << std::dec << std::endl;
                    std::cout << "    Смещение: 0x" << std::hex << healthOffset << std::dec << std::endl;
                    
                    // Сохраняем смещения
                    foundOffsets.m_iHealth = healthOffset;
                    foundOffsets.m_iTeamNum = 0x3E7;     // Запасное
                    foundOffsets.m_vecOrigin = 0x80;     // Запасное
                    foundOffsets.dwLocalPlayerPawn = 0x23C6268;  // Запасное
                    foundOffsets.dwLocalPlayerController = 0x23A0F30;  // Запасное
                    foundOffsets.dwViewMatrix = 0x23CB830;  // Запасное
                    foundOffsets.dwEntityList = 0x2571220;  // Запасное
                    
                    found = true;
                    break;
                }
            }
        }
        
        lastHealth = currentHealth;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 6. Сохраняем результат в secret/poc.trs
    if (found) {
        std::string jsonResult = R"({
    "timestamp": ")" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + R"(",
    "status": "found",
    "search_stats": {
        "total_attempts": )" + std::to_string(totalAttempts) + R"(,
        "final_candidates": 1
    },
    "offsets": {
        "dwEntityList": )" + std::to_string(foundOffsets.dwEntityList) + R"(,
        "dwLocalPlayerPawn": )" + std::to_string(foundOffsets.dwLocalPlayerPawn) + R"(,
        "dwLocalPlayerController": )" + std::to_string(foundOffsets.dwLocalPlayerController) + R"(,
        "dwViewMatrix": )" + std::to_string(foundOffsets.dwViewMatrix) + R"(,
        "m_iHealth": )" + std::to_string(foundOffsets.m_iHealth) + R"(,
        "m_iTeamNum": )" + std::to_string(foundOffsets.m_iTeamNum) + R"(,
        "m_vecOrigin": )" + std::to_string(foundOffsets.m_vecOrigin) + R"(
    }
})";
        
        if (WriteJsonToFile("secret/poc.trs", jsonResult)) {
            std::cout << "[✓] Результат сохранен в secret/poc.trs" << std::endl;
        } else {
            std::cout << "[!] Не удалось сохранить результат!" << std::endl;
        }
    }
    
    std::cout << std::endl << "Нажми Enter для выхода...";
    std::cin.get();
    
    CloseHandle(hProcess);
    return 0;
}