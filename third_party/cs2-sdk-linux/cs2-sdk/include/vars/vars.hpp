#pragma once

struct Vars_t {
    bool m_EnableESP{true};

    bool m_OtherBoxes{false};
    bool m_HostageBoxes{false};
    bool m_WeaponBoxes{true};
    bool m_ChickenBoxes{false};

    bool m_PlayerBoxes{true};
    bool m_PlayerNames{true};
    bool m_PlayerHealthBar{true};

    bool m_Use3DBoxes{false};
};

extern Vars_t g_Vars;
