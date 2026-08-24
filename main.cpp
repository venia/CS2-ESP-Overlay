#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

// ============================================
// 1. ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================

// Получить ID процесса по имени (без учета регистра)
DWORD GetProcessIdByName(const std::wstring& processName) {
    DWORD processId = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);
        
        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                // Сравниваем без учета регистра
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

// Получить список всех запущенных процессов (для отладки)
void ListAllProcesses() {
    std::cout << "   Запущенные процессы:" << std::endl;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);
        int count = 0;
        
        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                // Конвертируем wstring для вывода
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, processEntry.szExeFile, -1, NULL, 0, NULL, NULL);
                std::string name(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, processEntry.szExeFile, -1, &name[0], size_needed, NULL, NULL);
                
                std::cout << "      PID: " << processEntry.th32ProcessID 
                          << " | " << name;
                count++;
                if (count > 50) { // Показываем только первые 50
                    std::cout << " ... (и еще " << (count - 20) << " процессов)";
                    break;
                }
                std::cout << std::endl;
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
}

// Открыть процесс для чтения памяти
HANDLE OpenProcessForReading(DWORD processId) {
    return OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processId);
}

// Читаем память (шаблонная функция)
template<typename T>
bool ReadMemory(HANDLE process, uintptr_t address, T& value) {
    SIZE_T bytesRead;
    return ReadProcessMemory(process, (LPCVOID)address, &value, sizeof(T), &bytesRead) 
           && bytesRead == sizeof(T);
}

// Получаем базовый адрес модуля в процессе
uintptr_t GetModuleBaseAddress(HANDLE process, const std::wstring& moduleName) {
    uintptr_t baseAddress = 0;
    DWORD pid = GetProcessId(process);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W moduleEntry;
        moduleEntry.dwSize = sizeof(moduleEntry);
        
        if (Module32FirstW(snapshot, &moduleEntry)) {
            do {
                if (_wcsicmp(moduleName.c_str(), moduleEntry.szModule) == 0) {
                    baseAddress = (uintptr_t)moduleEntry.modBaseAddr;
                    break;
                }
            } while (Module32NextW(snapshot, &moduleEntry));
        }
        CloseHandle(snapshot);
    }
    return baseAddress;
}

// Сканируем память в поисках значения
std::vector<uintptr_t> ScanMemoryForValue(HANDLE process, uintptr_t startAddress, 
                                          size_t size, int targetValue) {
    std::vector<uintptr_t> foundAddresses;
    std::vector<BYTE> buffer(size);
    SIZE_T bytesRead;
    
    if (ReadProcessMemory(process, (LPCVOID)startAddress, buffer.data(), size, &bytesRead)) {
        for (size_t i = 0; i <= bytesRead - sizeof(int); i += sizeof(int)) {
            int value = *(int*)(buffer.data() + i);
            if (value == targetValue) {
                foundAddresses.push_back(startAddress + i);
            }
        }
    }
    return foundAddresses;
}

// Простая функция для вывода wstring в cout
void PrintWString(const std::wstring& wstr) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size_needed, NULL, NULL);
    std::cout << str;
}

// ============================================
// 2. ОСНОВНАЯ ПРОГРАММА
// ============================================

int main() {
    // Настройка консоли для UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    std::cout << "=== Memory Reader Demo (учебный пример) ===" << std::endl;
    std::cout << "Принцип работы внешних читов для CS2" << std::endl << std::endl;
    
    // Сначала покажем все процессы
    std::cout << "0. Проверяем запущенные процессы:" << std::endl;
    ListAllProcesses();
    std::cout << std::endl;
    
    // Шаг 1: Находим процесс (попробуем несколько вариантов)
    std::cout << "1. Ищем процесс..." << std::endl;
    
    std::vector<std::wstring> possibleNames = {
        L"CS2.exe",
        // L"NOTEPAD.EXE",
        // L"Notepad.exe"
    };
    
    DWORD pid = 0;
    std::wstring foundName;
    
    for (const auto& name : possibleNames) {
        pid = GetProcessIdByName(name);
        if (pid != 0) {
            foundName = name;
            break;
        }
    }
    
    if (pid == 0) {
        std::cout << "   [!] Процесс CS2.exe не найден!" << std::endl;
        std::cout << "   [!] Пожалуйста, запусти CS2.exe и попробуй снова." << std::endl;
        std::cout << "   Нажми Enter для выхода...";
        std::cin.get();
        return 1;
    }
    
    std::cout << "   [✓] Найден процесс: ";
    PrintWString(foundName);
    std::cout << " (PID: " << pid << ")" << std::endl;
    
    // Шаг 2: Открываем процесс
    HANDLE hProcess = OpenProcessForReading(pid);
    if (hProcess == NULL) {
        std::cout << "   [!] Не удалось открыть процесс. Попробуй запустить от администратора." << std::endl;
        std::cout << "   Нажми Enter для выхода...";
        std::cin.get();
        return 1;
    }
    std::cout << "2. Открыли процесс для чтения памяти" << std::endl;
    
    // Шаг 3: Получаем базовый адрес модуля
    // std::wstring moduleName = L"notepad.exe";
    std::wstring moduleName = foundName;
    uintptr_t moduleBase = GetModuleBaseAddress(hProcess, moduleName);
    if (moduleBase == 0) {
        std::cout << "   [!] Не удалось получить базовый адрес" << std::endl;
        CloseHandle(hProcess);
        std::cout << "   Нажми Enter для выхода...";
        std::cin.get();
        return 1;
    }
    std::cout << "3. Базовый адрес модуля: 0x" << std::hex << moduleBase << std::dec << std::endl;
    
    // Шаг 4: Читаем память
    std::cout << "4. Читаем память процесса..." << std::endl;
    
    int firstBytes = 0;
    if (ReadMemory(hProcess, moduleBase, firstBytes)) {
        std::cout << "   Первые 4 байта по базовому адресу: 0x" << std::hex << firstBytes << std::dec << std::endl;
        std::cout << "   (Это сигнатура MZ, которая есть у всех .exe файлов)" << std::endl;
    }
    
    // Шаг 5: Сканируем память
    std::cout << "5. Сканируем память в поисках значения 0x12345678..." << std::endl;
    
    size_t scanSize = 64 * 1024; // 64 KB
    auto found = ScanMemoryForValue(hProcess, moduleBase, scanSize, 0x12345678);
    
    if (!found.empty()) {
        std::cout << "   [✓] Найдено " << found.size() << " совпадений!" << std::endl;
        for (size_t i = 0; i < std::min(found.size(), size_t(5)); ++i) {
            std::cout << "      Адрес: 0x" << std::hex << found[i] << std::dec << std::endl;
        }
    } else {
        std::cout << "   Значение 0x12345678 не найдено (это нормально для демонстрации)" << std::endl;
    }
    
    // Шаг 6: Демонстрация чтения разных типов данных
    std::cout << "6. Демонстрация чтения разных типов данных:" << std::endl;
    
    int intValue = 0;
    uintptr_t testAddress = moduleBase + 0x1000;
    if (ReadMemory(hProcess, testAddress, intValue)) {
        std::cout << "   int по адресу 0x" << std::hex << testAddress 
                  << ": " << std::dec << intValue << std::endl;
    } else {
        std::cout << "   Не удалось прочитать int по адресу 0x" << std::hex << testAddress << std::dec << std::endl;
    }
    
    float floatValue = 0.0f;
    testAddress = moduleBase + 0x2000;
    if (ReadMemory(hProcess, testAddress, floatValue)) {
        std::cout << "   float по адресу 0x" << std::hex << testAddress 
                  << ": " << std::dec << floatValue << std::endl;
    } else {
        std::cout << "   Не удалось прочитать float по адресу 0x" << std::hex << testAddress << std::dec << std::endl;
    }
    
    // Закрываем хендл процесса
    CloseHandle(hProcess);
    std::cout << std::endl << "7. Готово! Нажми Enter для выхода..." << std::endl;
    std::cin.get();
    
    return 0;
}