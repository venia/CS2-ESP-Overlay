// Псевдокод, демонстрирующий логику чтения памяти
// 1. Получаем адрес базы клиентской части игры
uintptr_t clientBase = GetModuleBaseAddress("client.dll");

// 2. Читаем массив сущностей (игроков) из памяти
// Адрес = база + смещение (оффсет), которое нужно обновлять после каждого патча
uintptr_t entityList = ReadMemory<uintptr_t>(clientBase + offsets::dwEntityList);

for (int i = 0; i < 32; i++) {
    // 3. Получаем адрес конкретного игрока
    uintptr_t player = ReadMemory<uintptr_t>(entityList + i * 0x10);
    if (player == 0) continue;

    // 4. Проверяем, враг ли это (читаем флаг команды)
    int team = ReadMemory<int>(player + offsets::m_iTeamNum);
    if (team == localTeam) continue;

    // 5. Читаем позицию врага в 3D-мире
    Vector3 headPos = ReadMemory<Vector3>(player + offsets::m_vecOrigin);
    Vector3 footPos = headPos;
    footPos.z -= 70.0f; // Примерное смещение для ног

    // 6. Преобразуем мировые координаты в экранные
    Vector2 screenHead, screenFoot;
    if (WorldToScreen(headPos, screenHead) && WorldToScreen(footPos, screenFoot)) {
        // 7. Рисуем прямоугольник (ESP) на нашем оверлее
        DrawBox(screenHead.x, screenHead.y, screenFoot.x, screenFoot.y);
    }
}