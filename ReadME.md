                                        D:\Projects\c-cpp\NotepadReadMemory\main.exe
=== Memory Reader Demo (учебный пример) ===
Принцип работы внешних читов для CS2

0. Проверяем запущенные процессы:
   Запущенные процессы:
      PID: 0 | [System Process]
      PID: 4 | System
      PID: 272 | Registry
      PID: 764 | smss.exe
      PID: 1228 | csrss.exe
      PID: 1312 | wininit.exe
      PID: 1320 | csrss.exe
      PID: 1412 | services.exe
      PID: 1440 | winlogon.exe
      PID: 1484 | lsass.exe
      PID: 1624 | svchost.exe
      PID: 1660 | fontdrvhost.exe
      PID: 1656 | fontdrvhost.exe
      PID: 1748 | svchost.exe
      PID: 1796 | svchost.exe
      PID: 1860 | dwm.exe
      PID: 1224 | svchost.exe
      PID: 1232 | svchost.exe
      PID: 1324 | svchost.exe
      PID: 1304 | svchost.exe
      PID: 1080 | svchost.exe
      PID: 2092 | svchost.exe
      PID: 2100 | svchost.exe
      PID: 2140 | svchost.exe
      PID: 2156 | svchost.exe
      PID: 2164 | svchost.exe
      PID: 2252 | svchost.exe
      PID: 2372 | svchost.exe
      PID: 2392 | svchost.exe
      PID: 2820 | svchost.exe
      PID: 2860 | NVDisplay.Container.exe
      PID: 3000 | svchost.exe
      PID: 3084 | svchost.exe
      PID: 3092 | svchost.exe
      PID: 3100 | svchost.exe
      PID: 3120 | svchost.exe
      PID: 3144 | svchost.exe
      PID: 3152 | svchost.exe
      PID: 3304 | WUDFHost.exe
      PID: 3312 | Memory Compression
      PID: 3380 | svchost.exe
      PID: 3408 | svchost.exe
      PID: 3440 | svchost.exe
      PID: 3552 | svchost.exe
      PID: 3572 | svchost.exe
      PID: 3612 | svchost.exe
      PID: 3728 | svchost.exe
      PID: 3736 | QHActiveDefense.exe
      PID: 3744 | svchost.exe
      PID: 3904 | svchost.exe
      PID: 3944 | svchost.exe ... (и еще 31 процессов)
1. Ищем процесс...
   [✓] Найден процесс: CS2.exe (PID: 26008)
2. Открыли процесс для чтения памяти
3. Базовый адрес модуля: 0x7ff735c20000
4. Читаем память процесса...
   Первые 4 байта по базовому адресу: 0x905a4d
   (Это сигнатура MZ, которая есть у всех .exe файлов)
5. Сканируем память в поисках значения 0x12345678...
   Значение 0x12345678 не найдено (это нормально для демонстрации)
6. Демонстрация чтения разных типов данных:
   int по адресу 0x7ff735c21000: 957189448
   float по адресу 0x7ff735c22000: -2.77824e-32

7. Готово! Нажми Enter для выхода...
