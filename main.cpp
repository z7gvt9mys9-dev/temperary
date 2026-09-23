#include <string>

// Простой макрос для шифрования строк «на лету» во время компиляции.
// Ключ шифрования (например, 0x5A) меняет байты символов так, что в HxD они выглядят как мусор.
template <size_t N>
struct XorString {
    char data[N];
    
    constexpr XorString(const char* str) : data{} {
        for (size_t i = 0; i < N; ++i) {
            data[i] = str[i] ^ 0x5A; // 0x5A — наш секретный ключ XOR
        }
    }

    std::wstring to_wstring() const {
        std::string decrypted = std::string(data, N);
        for (size_t i = 0; i < N; ++i) {
            decrypted[i] ^= 0x5A; // Расшифровываем обратно в памяти при вызове
        }
        return std::wstring(decrypted.begin(), decrypted.end());
    }

    std::string to_string() const {
        std::string decrypted = std::string(data, N);
        for (size_t i = 0; i < N; ++i) {
            decrypted[i] ^= 0x5A;
        }
        return decrypted;
    }
};

// Макросы для удобного использования в коде
#define X_STR(str) (XorString<sizeof(str)>(str).to_string())
#define X_WSTR(str) (XorString<sizeof(str)>(str).to_wstring())


#include <iostream>
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string> // Добавлено для работы со строками текста ХП

#include "offsets.hpp"        
#include "client_dll.hpp"     

// Автоматически определяем разрешение экрана пользователя
const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

struct Vector3 { float x, y, z; };

// Структура матрицы 4х4 для Source 2
struct Matrix4x4 { float m[4][4]; };

DWORD GetProcessId(const wchar_t* procName) {
    DWORD procId = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W procEntry;
        procEntry.dwSize = sizeof(procEntry);
        if (Process32FirstW(hSnap, &procEntry)) {
            do {
                if (!_wcsicmp(procEntry.szExeFile, procName)) {
                    procId = procEntry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnap, &procEntry));
        }
        CloseHandle(hSnap);
    }
    return procId;
}

uintptr_t GetModuleBase(DWORD procId, const wchar_t* modName) {
    uintptr_t modBaseAddr = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, procId);
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W modEntry;
        modEntry.dwSize = sizeof(modEntry);
        if (Module32FirstW(hSnap, &modEntry)) {
            do {
                if (!_wcsicmp(modEntry.szModule, modName)) {
                    modBaseAddr = (uintptr_t)modEntry.modBaseAddr;
                    break;
                }
            } while (Module32NextW(hSnap, &modEntry));
        }
        CloseHandle(hSnap);
    }
    return modBaseAddr;
}

template <typename T>
T ReadMem(HANDLE hProcess, uintptr_t address) {
    T value;
    ReadProcessMemory(hProcess, (LPCVOID)address, &value, sizeof(T), NULL);
    return value;
}

// Математика перевода 3D координат игры в 2D пиксели монитора
bool WorldToScreen(Vector3 pos, Vector3& screen, Matrix4x4 matrix) {
    float clipW = pos.x * matrix.m[3][0] + pos.y * matrix.m[3][1] + pos.z * matrix.m[3][2] + matrix.m[3][3];

    if (clipW < 0.01f) 
        return false;

    float clipX = pos.x * matrix.m[0][0] + pos.y * matrix.m[0][1] + pos.z * matrix.m[0][2] + matrix.m[0][3];
    float clipY = pos.x * matrix.m[1][0] + pos.y * matrix.m[1][1] + pos.z * matrix.m[1][2] + matrix.m[1][3];

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    screen.x = (screenWidth / 2.0f) + (ndcX * screenWidth / 2.0f) + 0.5f;
    screen.y = (screenHeight / 2.0f) - (ndcY * screenHeight / 2.0f) + 0.5f;
    
    return true;
}

// Функция отрисовки рамки (боксов)
void DrawBorderBox(HDC hdc, int x, int y, int w, int h, int thickness, COLORREF color) {
    HBRUSH hBrush = CreateSolidBrush(color);
    RECT rectTop = { x, y, x + w, y + thickness };
    RECT rectBottom = { x, y + h - thickness, x + w, y + h };
    RECT rectLeft = { x, y, x + thickness, y + h };
    RECT rectRight = { x + w - thickness, y, x + w, y + h };

    FillRect(hdc, &rectTop, hBrush);
    FillRect(hdc, &rectBottom, hBrush);
    FillRect(hdc, &rectLeft, hBrush);
    FillRect(hdc, &rectRight, hBrush);

    DeleteObject(hBrush);
}

void DrawHealthBar(HDC hdc, int boxX, int boxY, int boxHeight, int health) {
    int r = (255 * (100 - health)) / 100;
    int g = (255 * health) / 100;
    COLORREF hpColor = RGB(r, g, 0);

    HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
    int barWidth = 4;
    int barX = boxX - barWidth - 4;
    RECT bgRect = { barX, boxY, barX + barWidth, boxY + boxHeight };
    FillRect(hdc, &bgRect, bgBrush);
    DeleteObject(bgBrush);

    HBRUSH hpBrush = CreateSolidBrush(hpColor);
    int hpHeight = (boxHeight * health) / 100;
    RECT hpRect = { barX, boxY + boxHeight - hpHeight, barX + barWidth, boxY + boxHeight };
    FillRect(hdc, &hpRect, hpBrush);
    DeleteObject(hpBrush);

    if (health < 100) {
        std::wstring hpStr = std::to_wstring(health);
        SetTextColor(hdc, hpColor);
        SetBkMode(hdc, TRANSPARENT);
        TextOutW(hdc, barX - 25, boxY + boxHeight - hpHeight, hpStr.c_str(), hpStr.length());
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setlocale(LC_ALL, "Russian");
    std::cout << "[+] Запуск самописного ESP (Боксы + ХП) для CS2...\n";
    std::cout << "[!] Разрешение экрана: " << screenWidth << "x" << screenHeight << "\n";
    std::cout << "[!] ВАЖНО: Игра должна быть в оконном режиме!\n";

    using namespace cs2_dumper::offsets::client_dll;
    using namespace cs2_dumper::schemas::client_dll;

    DWORD procId = GetProcessId(L"cs2.exe");
    if (!procId) {
        std::cout << "[-] Ошибка: Запустите CS2.\n";
        system("pause");
        return 1;
    }

    HANDLE hProcess = OpenProcess(PROCESS_VM_READ, FALSE, procId);
    uintptr_t clientModule = GetModuleBase(procId, L"client.dll");

    HDC hdc = GetDC(NULL); 

    std::cout << "[+] Успешно! ESP активирован. Нажмите END для выхода.\n";

    while (!GetAsyncKeyState(VK_END)) {

        uintptr_t localPlayerPawn = ReadMem<uintptr_t>(hProcess, clientModule + dwLocalPlayerPawn);
        if (!localPlayerPawn) continue;

        int localTeam = ReadMem<int>(hProcess, localPlayerPawn + C_BaseEntity::m_iTeamNum);
        uintptr_t entityList = ReadMem<uintptr_t>(hProcess, clientModule + dwEntityList);
        if (!entityList) continue;

        uintptr_t listEntry = ReadMem<uintptr_t>(hProcess, entityList + 0x10);
        if (!listEntry) continue;

        Matrix4x4 viewMatrix = ReadMem<Matrix4x4>(hProcess, clientModule + dwViewMatrix);

        for (int i = 0; i < 64; i++) {
            uintptr_t currentController = ReadMem<uintptr_t>(hProcess, listEntry + (i * 0x78));
            if (!currentController) continue;

            uint32_t playerPawnHandle = ReadMem<uint32_t>(hProcess, currentController + CCSPlayerController::m_hPlayerPawn);
            if (!playerPawnHandle) continue;

            uintptr_t playerPawnListEntry = ReadMem<uintptr_t>(hProcess, entityList + (8 * ((playerPawnHandle & 0x7FFF) >> 9) + 0x10));
            if (!playerPawnListEntry) continue;

            uintptr_t currentPawn = ReadMem<uintptr_t>(hProcess, playerPawnListEntry + (120 * (playerPawnHandle & 0x1FF)));
            if (!currentPawn || currentPawn == localPlayerPawn) continue;

            int health = ReadMem<int>(hProcess, currentPawn + C_BaseEntity::m_iHealth);
            int team = ReadMem<int>(hProcess, currentPawn + C_BaseEntity::m_iTeamNum);

            if (health > 0 && health <= 100 && team != localTeam) {
                Vector3 origin = ReadMem<Vector3>(hProcess, currentPawn + C_BasePlayerPawn::m_vOldOrigin);
                Vector3 head = { origin.x, origin.y, origin.z + 72.0f }; 

                Vector3 screenOrigin, screenHead;

                if (WorldToScreen(origin, screenOrigin, viewMatrix) && WorldToScreen(head, screenHead, viewMatrix)) {
                    
                    int boxHeight = static_cast<int>(screenOrigin.y - screenHead.y);
                    int boxWidth = boxHeight / 2;
                    int boxX = static_cast<int>(screenHead.x - (boxWidth / 2));
                    int boxY = static_cast<int>(screenHead.y);

                    DrawBorderBox(hdc, boxX, boxY, boxWidth, boxHeight, 2, RGB(255, 0, 0));
                    DrawHealthBar(hdc, boxX, boxY, boxHeight, health);
                }
            }
        }
        Sleep(10); 
    }

    std::cout << "[+] Выход. Очистка ресурсов.\n";
    ReleaseDC(NULL, hdc);
    CloseHandle(hProcess);
    return 0;
}
