// pattern_scanning.cpp
// Advanced Memory Scanner with Heuristic + Pattern Detection
// Compiled: g++ -g pattern_scanning.cpp -o pattern_scanning.exe -lgdi32 -luser32 -lpsapi -static

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <psapi.h>

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

struct Candidate {
    uintptr_t address;
    int currentValue;
    int previousValue;
    int stableCount;
    int type;          // 0 = int, 1 = short, 2 = float
    int regionIndex;   // Which memory region
    bool verified;     // Has been verified
};

struct MemoryRegion {
    uintptr_t start;
    uintptr_t end;
    size_t size;
    DWORD protect;
    bool isReadable;
};

struct ScanStats {
    int totalAttempts;
    int totalCandidatesFound;
    int currentCandidates;
    int filteredOut;
    int verifiedCount;
    double scanTimeMs;
    int regionsScanned;      
    int totalRegions;        
    std::vector<int> candidatesHistory;
};

// ============================================
// GLOBAL VARIABLES
// ============================================

std::vector<Candidate> g_candidates;
std::vector<MemoryRegion> g_regions;
ScanStats g_stats = {};
bool g_found = false;
Offsets g_foundOffsets = {};
std::chrono::steady_clock::time_point g_scanStart;

// ============================================
// HELPER FUNCTIONS
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
// JSON PARSING (Simple)
// ============================================

int ExtractIntFromJSON(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return 0;
    
    pos += searchKey.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) pos++;
    
    size_t endPos = pos;
    while (endPos < json.length() && (isdigit(json[endPos]) || json[endPos] == '-')) endPos++;
    
    if (pos == endPos) return 0;
    return std::stoi(json.substr(pos, endPos - pos));
}

std::string MakeResultJSON(const Offsets& offsets, const ScanStats& stats) {
    std::stringstream ss;
    ss << "{\n";
    ss << "    \"timestamp\": \"" << std::chrono::system_clock::now().time_since_epoch().count() << "\",\n";
    ss << "    \"status\": \"found\",\n";
    ss << "    \"search_stats\": {\n";
    ss << "        \"total_attempts\": " << stats.totalAttempts << ",\n";
    ss << "        \"total_candidates_found\": " << stats.totalCandidatesFound << ",\n";
    ss << "        \"final_candidates\": " << stats.currentCandidates << ",\n";
    ss << "        \"filtered_out\": " << stats.filteredOut << ",\n";
    ss << "        \"scan_time_ms\": " << stats.scanTimeMs << "\n";
    ss << "    },\n";
    ss << "    \"offsets\": {\n";
    ss << "        \"dwEntityList\": " << offsets.dwEntityList << ",\n";
    ss << "        \"dwLocalPlayerPawn\": " << offsets.dwLocalPlayerPawn << ",\n";
    ss << "        \"dwLocalPlayerController\": " << offsets.dwLocalPlayerController << ",\n";
    ss << "        \"dwViewMatrix\": " << offsets.dwViewMatrix << ",\n";
    ss << "        \"m_iHealth\": " << offsets.m_iHealth << ",\n";
    ss << "        \"m_iTeamNum\": " << offsets.m_iTeamNum << ",\n";
    ss << "        \"m_vecOrigin\": " << offsets.m_vecOrigin << "\n";
    ss << "    }\n";
    ss << "}";
    return ss.str();
}

// ============================================
// FILE OPERATIONS (Atomic)
// ============================================

bool WriteFileSafe(const std::string& filename, const std::string& data) {
    std::string tempFile = filename + ".tmp";
    
    // 1. Пишем во временный файл
    std::ofstream out(tempFile);
    if (!out.is_open()) return false;
    out << data;
    out.close();
    
    // 2. Удаляем старый файл (если есть)
    DeleteFileA(filename.c_str());
    
    // 3. Переименовываем временный в основной
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

// ============================================
// MEMORY REGION DETECTION
// ============================================

std::vector<MemoryRegion> GetReadableRegions(HANDLE hProcess, uintptr_t baseAddress, size_t maxSize) {
    std::vector<MemoryRegion> regions;
    uintptr_t currentAddress = baseAddress;
    uintptr_t endAddress = baseAddress + maxSize;
    
    while (currentAddress < endAddress) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQueryEx(hProcess, (LPCVOID)currentAddress, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        
        // Only regions that are committed and readable
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect & PAGE_READWRITE || mbi.Protect & PAGE_READONLY)) {
            
            MemoryRegion region;
            region.start = (uintptr_t)mbi.BaseAddress;
            region.end = region.start + mbi.RegionSize;
            region.size = mbi.RegionSize;
            region.protect = mbi.Protect;
            region.isReadable = true;
            regions.push_back(region);
        }
        
        currentAddress = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    
    return regions;
}

// ============================================
// ADVANCED MEMORY SCANNING
// ============================================

std::vector<Candidate> ScanRegionForValue(HANDLE hProcess, const MemoryRegion& region, 
                                          int targetValue, int regionIndex) {
    std::vector<Candidate> result;
    std::vector<BYTE> buffer(region.size);
    SIZE_T bytesRead;
    
    if (!ReadProcessMemory(hProcess, (LPCVOID)region.start, buffer.data(), region.size, &bytesRead)) {
        return result;
    }
    
    // Scan as int (4 bytes)
    for (size_t i = 0; i <= bytesRead - sizeof(int); i += 1) {
        int value = *(int*)(buffer.data() + i);
        if (value == targetValue) {
            Candidate cand;
            cand.address = region.start + i;
            cand.currentValue = targetValue;
            cand.previousValue = targetValue;
            cand.stableCount = 0;
            cand.type = 0; // int
            cand.regionIndex = regionIndex;
            cand.verified = false;
            result.push_back(cand);
        }
    }
    
    // Scan as short (2 bytes)
    for (size_t i = 0; i <= bytesRead - sizeof(short); i += 1) {
        short value = *(short*)(buffer.data() + i);
        if (value == targetValue) {
            Candidate cand;
            cand.address = region.start + i;
            cand.currentValue = targetValue;
            cand.previousValue = targetValue;
            cand.stableCount = 0;
            cand.type = 1; // short
            cand.regionIndex = regionIndex;
            cand.verified = false;
            result.push_back(cand);
        }
    }
    
    // Scan as float (4 bytes) - only if value is in valid range
    if (targetValue >= 0 && targetValue <= 100) {
        float floatTarget = (float)targetValue;
        for (size_t i = 0; i <= bytesRead - sizeof(float); i += 1) {
            float value = *(float*)(buffer.data() + i);
            // Check if float is close to target (with small epsilon)
            if (std::abs(value - floatTarget) < 0.01f) {
                Candidate cand;
                cand.address = region.start + i;
                cand.currentValue = (int)(value + 0.5f);
                cand.previousValue = (int)(value + 0.5f);
                cand.stableCount = 0;
                cand.type = 2; // float
                cand.regionIndex = regionIndex;
                cand.verified = false;
                result.push_back(cand);
            }
        }
    }
    
    return result;
}

std::vector<Candidate> FilterCandidates(HANDLE hProcess, const std::vector<Candidate>& oldCandidates, int newValue) {
    std::vector<Candidate> result;
    
    for (const auto& cand : oldCandidates) {
        if (cand.type == 0) {
            int currentValue = 0;
            if (ReadMemory(hProcess, cand.address, currentValue)) {
                if (currentValue == newValue) {
                    Candidate newCand = cand;
                    newCand.currentValue = currentValue;
                    newCand.previousValue = cand.currentValue;
                    result.push_back(newCand);
                }
            }
        } else if (cand.type == 1) {
            short currentValue = 0;
            if (ReadMemory(hProcess, cand.address, currentValue)) {
                if (currentValue == newValue) {
                    Candidate newCand = cand;
                    newCand.currentValue = currentValue;
                    newCand.previousValue = cand.currentValue;
                    result.push_back(newCand);
                }
            }
        } else if (cand.type == 2) {
            float currentValue = 0;
            if (ReadMemory(hProcess, cand.address, currentValue)) {
                float floatTarget = (float)newValue;
                if (std::abs(currentValue - floatTarget) < 0.01f) {
                    Candidate newCand = cand;
                    newCand.currentValue = (int)(currentValue + 0.5f);
                    newCand.previousValue = cand.currentValue;
                    result.push_back(newCand);
                }
            }
        }
    }
    
    return result;
}

// ============================================
// VERIFICATION
// ============================================

bool VerifyCandidate(HANDLE hProcess, const Candidate& cand) {
    // Check if value is in valid range
    if (cand.currentValue < 0 || cand.currentValue > 100) {
        return false;
    }
    
    // Try to read as different types to see if it's consistent
    int intValue = 0;
    short shortValue = 0;
    float floatValue = 0;
    
    if (ReadMemory(hProcess, cand.address, intValue)) {
        if (intValue == cand.currentValue) {
            return true;
        }
    }
    
    if (ReadMemory(hProcess, cand.address, shortValue)) {
        if (shortValue == cand.currentValue) {
            return true;
        }
    }
    
    if (ReadMemory(hProcess, cand.address, floatValue)) {
        if ((int)(floatValue + 0.5f) == cand.currentValue) {
            return true;
        }
    }
    
    return false;
}

// ============================================
// DISPLAY FUNCTIONS
// ============================================

void PrintHeader() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     CS2 PATTERN SCANNER v3 - Advanced Memory Scanner       ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
}

void PrintStats() {
    std::cout << "┌──────────────────────────────────────────────────────────────┐" << std::endl;
    std::cout << "│ SCAN STATISTICS                                             │" << std::endl;
    std::cout << "├──────────────────────────────────────────────────────────────┤" << std::endl;
    std::cout << "│ Total Attempts     : " << std::setw(30) << std::left << g_stats.totalAttempts << "│" << std::endl;
    std::cout << "│ Candidates Found   : " << std::setw(30) << std::left << g_stats.totalCandidatesFound << "│" << std::endl;
    std::cout << "│ Current Candidates : " << std::setw(30) << std::left << g_stats.currentCandidates << "│" << std::endl;
    std::cout << "│ Filtered Out       : " << std::setw(30) << std::left << g_stats.filteredOut << "│" << std::endl;
    std::cout << "│ Verified Count     : " << std::setw(30) << std::left << g_stats.verifiedCount << "│" << std::endl;
    std::cout << "│ Scan Time (ms)     : " << std::setw(30) << std::left << std::fixed << std::setprecision(2) << g_stats.scanTimeMs << "│" << std::endl;
    std::cout << "└──────────────────────────────────────────────────────────────┘" << std::endl;
}

void PrintCandidates(const std::vector<Candidate>& candidates, int maxShow = 5) {
    if (candidates.empty()) {
        std::cout << "│ No candidates found                                       │" << std::endl;
        return;
    }
    
    std::cout << "┌──────────────────────────────────────────────────────────────┐" << std::endl;
    std::cout << "│ CANDIDATES (" << candidates.size() << " total)                                   │" << std::endl;
    std::cout << "├──────────────────────────────────────────────────────────────┤" << std::endl;
    
    int show = std::min((int)candidates.size(), maxShow);
    for (int i = 0; i < show; i++) {
        const auto& cand = candidates[i];
        const char* typeName = cand.type == 0 ? "int" : (cand.type == 1 ? "short" : "float");
        std::cout << "│ " << std::hex << "0x" << cand.address << std::dec 
                  << " | val: " << std::setw(3) << cand.currentValue 
                  << " | type: " << std::setw(5) << typeName
                  << " | region: " << std::setw(3) << cand.regionIndex 
                  << " | verified: " << (cand.verified ? "YES" : "NO ") << " │" << std::endl;
    }
    
    if (candidates.size() > maxShow) {
        std::cout << "│ ... and " << (candidates.size() - maxShow) << " more candidates                    │" << std::endl;
    }
    std::cout << "└──────────────────────────────────────────────────────────────┘" << std::endl;
}

void WriteProgressToFile(int currentHealth, int totalAttempts, int candidates, 
                         int regionsScanned, int totalRegions, bool found) {
    
    // 1. Читаем существующий ccs.trs
    std::string existingData = ReadFileSafe("secret/ccs.trs");
    
    // 2. Парсим team и position из существующего файла
    int team = 0;
    float posX = 0, posY = 0, posZ = 0;
    
    if (!existingData.empty()) {
        // Извлекаем team
        size_t teamPos = existingData.find("\"team\":");
        if (teamPos != std::string::npos) {
            teamPos = existingData.find(":", teamPos) + 1;
            while (existingData[teamPos] == ' ' || existingData[teamPos] == '\n') teamPos++;
            size_t endPos = existingData.find_first_of(",}", teamPos);
            team = std::stoi(existingData.substr(teamPos, endPos - teamPos));
        }
        
        // Извлекаем position
        size_t xPos = existingData.find("\"x\":");
        if (xPos != std::string::npos) {
            xPos = existingData.find(":", xPos) + 1;
            while (existingData[xPos] == ' ' || existingData[xPos] == '\n') xPos++;
            size_t endPos = existingData.find_first_of(",}", xPos);
            posX = std::stof(existingData.substr(xPos, endPos - xPos));
        }
        
        size_t yPos = existingData.find("\"y\":");
        if (yPos != std::string::npos) {
            yPos = existingData.find(":", yPos) + 1;
            while (existingData[yPos] == ' ' || existingData[yPos] == '\n') yPos++;
            size_t endPos = existingData.find_first_of(",}", yPos);
            posY = std::stof(existingData.substr(yPos, endPos - yPos));
        }
        
        size_t zPos = existingData.find("\"z\":");
        if (zPos != std::string::npos) {
            zPos = existingData.find(":", zPos) + 1;
            while (existingData[zPos] == ' ' || existingData[zPos] == '\n') zPos++;
            size_t endPos = existingData.find_first_of(",}", zPos);
            posZ = std::stof(existingData.substr(zPos, endPos - zPos));
        }
    }
    
    // 3. Формируем JSON с сохранением team и position
    std::stringstream ss;
    ss << "{\n";
    ss << "    \"timestamp\": \"" << std::chrono::system_clock::now().time_since_epoch().count() << "\",\n";
    ss << "    \"health\": " << currentHealth << ",\n";
    ss << "    \"team\": " << team << ",\n";  // ← Сохраняем оригинальную команду!
    ss << "    \"position\": {\n";
    ss << "        \"x\": " << posX << ",\n";   // ← Сохраняем оригинальную позицию!
    ss << "        \"y\": " << posY << ",\n";
    ss << "        \"z\": " << posZ << "\n";
    ss << "    },\n";
    ss << "    \"search\": {\n";
    ss << "        \"attempts\": " << totalAttempts << ",\n";
    ss << "        \"status\": \"" << (found ? "found" : "scanning") << "\",\n";
    ss << "        \"candidates\": " << candidates << ",\n";
    ss << "        \"regions_scanned\": " << regionsScanned << ",\n";
    ss << "        \"total_regions\": " << totalRegions << "\n";
    ss << "    }\n";
    ss << "}";
    
    WriteFileSafe("secret/ccs.trs", ss.str());
}

// ============================================
// MAIN
// ============================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    PrintHeader();
    
    CreateDirectoryIfNotExists("secret");
    
    // Find CS2 process
    DWORD pid = GetProcessIdByName(L"cs2.exe");
    if (pid == 0) {
        std::cout << "[ERROR] CS2 is not running!" << std::endl;
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] CS2 PID: " << pid << std::endl;
    
    // Open process
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        std::cout << "[ERROR] Failed to open process! Run as Administrator." << std::endl;
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] Process opened" << std::endl;
    
    // Get client.dll base
    uintptr_t clientBase = GetModuleBaseAddress(pid, L"client.dll");
    if (clientBase == 0) {
        std::cout << "[ERROR] client.dll not found!" << std::endl;
        CloseHandle(hProcess);
        std::cin.get();
        return 1;
    }
    std::cout << "[OK] client.dll: 0x" << std::hex << clientBase << std::dec << std::endl;
    
    // Get memory regions
    size_t clientSize = 0;
    HMODULE hModule = (HMODULE)clientBase;
    MODULEINFO moduleInfo;
    if (GetModuleInformation(hProcess, hModule, &moduleInfo, sizeof(moduleInfo))) {
        clientSize = moduleInfo.SizeOfImage;
        std::cout << "[OK] client.dll size: 0x" << std::hex << clientSize << std::dec << " bytes" << std::endl;
    } else {
        // Fallback: пробуем VirtualQuery
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQueryEx(hProcess, (LPCVOID)clientBase, &mbi, sizeof(mbi))) {
            clientSize = mbi.RegionSize;
        }
        std::cout << "[OK] client.dll size (fallback): 0x" << std::hex << clientSize << std::dec << " bytes" << std::endl;
    }
    
    // Get readable regions
    g_regions = GetReadableRegions(hProcess, clientBase, clientSize);
    g_stats.totalRegions = (int)g_regions.size();
    std::cout << "[OK] Found " << g_regions.size() << " readable memory regions" << std::endl;
    std::cout << std::endl;
    
    // Fallback offsets
    g_foundOffsets.dwEntityList = 0x2571220;
    g_foundOffsets.dwLocalPlayerPawn = 0x23C6268;
    g_foundOffsets.dwLocalPlayerController = 0x23A0F30;
    g_foundOffsets.dwViewMatrix = 0x23CB830;
    g_foundOffsets.m_iHealth = 0x34C;
    g_foundOffsets.m_iTeamNum = 0x3E7;
    g_foundOffsets.m_vecOrigin = 0x80;
    
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "INSTRUCTIONS:" << std::endl;
    std::cout << "1. Start a match with bots" << std::endl;
    std::cout << "2. Take damage (change your health)" << std::endl;
    std::cout << "3. Scanner will find the health offset automatically" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Waiting for data from cs_main.exe..." << std::endl;
    std::cout << "Make sure cs_main.exe is running and writing to secret/ccs.trs" << std::endl;
    std::cout << std::endl;
    
    bool found = false;
    int lastHealth = -1;
    bool firstScan = true;
    int scanCount = 0;
    bool hasInitialCandidates = false;
    
    g_scanStart = std::chrono::steady_clock::now();
    
    while (!found) {
        std::string jsonData = ReadFileSafe("secret/ccs.trs");
        if (jsonData.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        
        int currentHealth = ExtractIntFromJSON(jsonData, "health");
        
        if (currentHealth > 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        
        scanCount++;
        
        // Health changed or first scan
        if (currentHealth != lastHealth || firstScan) {
            firstScan = false;
            
            if (lastHealth != -1 && currentHealth != lastHealth) {
                g_stats.totalAttempts++;
            }
            
            // First scan after health changed
            if (g_candidates.empty() && lastHealth != -1 && !hasInitialCandidates) {
                auto scanStart = std::chrono::steady_clock::now();
                
                std::cout << "[SCAN] Starting memory scan for value: " << currentHealth << std::endl;
                std::cout << "       Scanning " << g_regions.size() << " regions..." << std::endl;
                
                int totalFound = 0;
                for (size_t i = 0; i < g_regions.size(); i++) {
                    auto regionCandidates = ScanRegionForValue(hProcess, g_regions[i], currentHealth, (int)i);
                    totalFound += regionCandidates.size();
                    g_candidates.insert(g_candidates.end(), regionCandidates.begin(), regionCandidates.end());
                    g_stats.regionsScanned = (int)i + 1;
                }
                
                auto scanEnd = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(scanEnd - scanStart);
                g_stats.scanTimeMs = (double)elapsed.count();
                
                g_stats.totalCandidatesFound = totalFound;
                g_stats.currentCandidates = totalFound;
                
                std::cout << "[SCAN] Found " << totalFound << " candidates" << std::endl;
                std::cout << "[SCAN] Scan time: " << elapsed.count() << " ms" << std::endl;
                std::cout << std::endl;
                
                hasInitialCandidates = true;
                PrintStats();
                PrintCandidates(g_candidates);
                std::cout << std::endl;
            }
            // Filter candidates
            else if (!g_candidates.empty() && currentHealth != lastHealth) {
                auto oldSize = g_candidates.size();
                
                std::cout << "[FILTER] Filtering candidates from " << currentHealth << std::endl;
                
                g_candidates = FilterCandidates(hProcess, g_candidates, currentHealth);
                g_stats.filteredOut += (oldSize - g_candidates.size());
                g_stats.currentCandidates = g_candidates.size();
                
                // Verify remaining candidates
                int verified = 0;
                for (auto& cand : g_candidates) {
                    if (VerifyCandidate(hProcess, cand)) {
                        cand.verified = true;
                        verified++;
                    }
                }
                g_stats.verifiedCount = verified;
                
                std::cout << "[FILTER] Remaining: " << g_candidates.size() << " candidates" << std::endl;
                std::cout << "[FILTER] Verified: " << verified << " candidates" << std::endl;
                std::cout << std::endl;
                
                PrintStats();
                PrintCandidates(g_candidates);
                std::cout << std::endl;
                
                // Check if we found a single candidate
                if (g_candidates.size() == 1) {
                    uintptr_t healthAddress = g_candidates[0].address;
                    uintptr_t healthOffset = healthAddress - clientBase;
                    
                    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
                    std::cout << "║                    HEALTH FOUND!                            ║" << std::endl;
                    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;
                    std::cout << "  Address  : 0x" << std::hex << healthAddress << std::dec << std::endl;
                    std::cout << "  Offset   : 0x" << std::hex << healthOffset << std::dec << std::endl;
                    std::cout << "  Type     : " << (g_candidates[0].type == 0 ? "int" : (g_candidates[0].type == 1 ? "short" : "float")) << std::endl;
                    std::cout << "  Verified : YES" << std::endl;
                    std::cout << std::endl;
                    
                    g_foundOffsets.m_iHealth = healthOffset;
                    found = true;
                    break;
                }
                
                // If we have few candidates but more than 1, show detailed info
                if (g_candidates.size() > 1 && g_candidates.size() <= 10) {
                    std::cout << "[INFO] Few candidates remaining. Try to change health again." << std::endl;
                    std::cout << "       " << g_candidates.size() << " candidates left." << std::endl;
                    std::cout << std::endl;
                }
            }
            
            lastHealth = currentHealth;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        WriteProgressToFile(
            currentHealth,
            g_stats.totalAttempts,
            (int)g_candidates.size(),
            g_stats.regionsScanned,
            g_stats.totalRegions,
            found
        );
    }
    
    // Save result
    if (found) {
        auto endTime = std::chrono::steady_clock::now();
        auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - g_scanStart);
        g_stats.scanTimeMs = (double)totalTime.count();
        
        std::string jsonResult = MakeResultJSON(g_foundOffsets, g_stats);
        
        if (WriteFileSafe("secret/poc.trs", jsonResult)) {
            std::cout << "[OK] Result saved to secret/poc.trs" << std::endl;
            std::cout << std::endl;
            std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
            std::cout << "FINAL STATISTICS:" << std::endl;
            PrintStats();
            std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
        } else {
            std::cout << "[ERROR] Failed to save result!" << std::endl;
        }
    }
    
    std::cout << std::endl << "Press Enter to exit...";
    std::cin.get();
    
    CloseHandle(hProcess);
    return 0;
}