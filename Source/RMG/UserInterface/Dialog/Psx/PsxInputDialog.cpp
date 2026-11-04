#include "PsxInputDialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTimer>

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Emulation.hpp>

extern void FbneoPsxInputReloadSettings(void);
#include "Utilities/QtKeyToSdl3Key.hpp"

#include <SDL3/SDL.h>
#include <cmath>

namespace UserInterface
{
namespace Dialog
{

static const char* kSection = "RMG PS1 Input - Global";

static const char* kButtonNames[14] = {
    "PS1 Cross", "PS1 Circle", "PS1 Square", "PS1 Triangle",
    "PS1 L1", "PS1 R1", "PS1 L2", "PS1 R2",
    "PS1 Start", "PS1 Select",
    "PS1 D-Pad Up", "PS1 D-Pad Down", "PS1 D-Pad Left", "PS1 D-Pad Right",
};

static const int kDefaultScancodes[14] = {
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
};

enum BindingSource
{
    Source_Keyboard       = -1,
    Source_GamepadButton  = 0,
    Source_GamepadAxis    = 1,
    Source_JoystickButton = 2,
    Source_JoystickAxis   = 3,
    Source_JoystickHat    = 4,
};

struct Binding
{
    int type  = Source_Keyboard;
    int data  = 0;
    int extra = 0;
};

PsxInputDialog::PsxInputDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("PlayStation Input"));
    setModal(true);
    resize(520, 480);

    auto* layout = new QVBoxLayout(this);

    // Device selection row
    auto* deviceRow = new QHBoxLayout();
    auto* playerLabel = new QLabel(tr("Player:"), this);
    auto* playerCombo = new QComboBox(this);
    for (int p = 0; p < 4; p++)
    {
        playerCombo->addItem(tr("P%1").arg(p + 1));
    }
    auto* deviceLabel = new QLabel(tr("Device:"), this);
    auto* deviceCombo = new QComboBox(this);
    deviceCombo->addItem(tr("None"), 0);
    deviceCombo->addItem(tr("Keyboard"), 1);
    deviceCombo->addItem(tr("Gamepad"), 2);
    deviceRow->addWidget(playerLabel);
    deviceRow->addWidget(playerCombo);
    deviceRow->addWidget(deviceLabel);
    deviceRow->addWidget(deviceCombo);
    layout->addLayout(deviceRow);

    for (int p = 0; p < 4; p++)
    {
        std::string section = "RMG PS1 Device - P" + std::to_string(p);
        m_deviceType[p] = CoreSettingsGetIntValue(SettingsID::FBNeo_DeviceType, section);
    }

    deviceCombo->setCurrentIndex(deviceCombo->findData(m_deviceType[0]));
    connect(playerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, deviceCombo](int idx) {
        deviceCombo->setCurrentIndex(deviceCombo->findData(m_deviceType[idx]));
    });
    connect(deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, playerCombo, deviceCombo](int) {
        m_deviceType[playerCombo->currentIndex()] = deviceCombo->currentData().toInt();
    });

    // Table
    m_table = new QTableWidget(14, 2, this);
    m_table->setHorizontalHeaderLabels({tr("Button"), tr("Binding")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int i = 0; i < 14; i++)
    {
        m_table->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(kButtonNames[i])));
        auto* btn = new QPushButton(tr("Bind"), this);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QPushButton::clicked, this, [this, i]() { this->startCapture(i); });
        m_table->setCellWidget(i, 1, btn);
    }
    layout->addWidget(m_table);

    m_status = new QLabel(tr("Click 'Bind' then press a key or button."), this);
    layout->addWidget(m_status);

    auto* saveBtn = new QPushButton(tr("Save"), this);
    connect(saveBtn, &QPushButton::clicked, this, [this]() { this->saveBindings(); this->accept(); });
    layout->addWidget(saveBtn);

    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
    {
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }

    m_names.clear();
    for (int i = 0; i < 14; i++)
    {
        m_names.push_back(QString::fromUtf8(kButtonNames[i]));
    }
    m_types.assign(14, Source_Keyboard);
    m_data.assign(14, 0);
    m_extra.assign(14, 0);
    for (int i = 0; i < 14; i++)
    {
        m_data[i] = kDefaultScancodes[i];
    }

    // load existing
    const std::vector<int> types = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingTypes, kSection);
    const std::vector<int> data = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingData, kSection);
    const std::vector<int> extra = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingExtraData, kSection);
    const std::vector<std::string> names = CoreSettingsGetStringListValue(SettingsID::FBNeo_BindingNames, kSection);
    if (names.size() == m_names.size() && types.size() == m_types.size() &&
        data.size() == m_data.size() && extra.size() == m_extra.size())
    {
        for (size_t i = 0; i < m_data.size(); i++)
        {
            if (!names[i].empty())
            {
                m_types[i] = types[i];
                m_data[i] = data[i];
                m_extra[i] = extra[i];
            }
        }
    }

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(16);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        if (m_capturing < 0) { m_pollTimer->stop(); return; }
        if (this->pollCaptureDevices())
        {
            // commit
            int idx = m_capturing;
            m_types[idx] = m_capturedBinding.type;
            m_data[idx] = m_capturedBinding.data;
            m_extra[idx] = m_capturedBinding.extra;
            stopCapture();
            refreshTable();
            if (m_status != nullptr)
            {
                m_status->setText(tr("Bound. Click 'Bind' for another button."));
            }
        }
    });

    refreshTable();
}

PsxInputDialog::~PsxInputDialog()
{
    closeCaptureDevices();
}

void PsxInputDialog::loadBindings()
{
}

void PsxInputDialog::saveBindings()
{
    std::vector<std::string> names;
    for (const QString& n : m_names)
    {
        names.push_back(n.toStdString());
    }
    std::vector<int> types(m_types.begin(), m_types.end());
    std::vector<int> data(m_data.begin(), m_data.end());
    std::vector<int> extra(m_extra.begin(), m_extra.end());

    CoreSettingsSetValue(SettingsID::FBNeo_BindingNames, kSection, names);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingTypes, kSection, types);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingData, kSection, data);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingExtraData, kSection, extra);

    for (int p = 0; p < 4; p++)
    {
        std::string section = "RMG PS1 Device - P" + std::to_string(p);
        CoreSettingsSetValue(SettingsID::FBNeo_DeviceType, section, m_deviceType[p]);
    }

    CoreSettingsSave();

    FbneoPsxInputReloadSettings();
}

void PsxInputDialog::refreshTable()
{
    for (int i = 0; i < 14; i++)
    {
        auto* btn = qobject_cast<QPushButton*>(m_table->cellWidget(i, 1));
        if (btn != nullptr)
        {
            QString label;
            switch (m_types[i])
            {
                case Source_Keyboard:
                {
                    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(m_data[i]));
                    label = (name != nullptr && name[0] != '\0') ? QString::fromUtf8(name) : tr("Unbound");
                    break;
                }
                case Source_GamepadButton:
                    label = tr("Gamepad Button %1").arg(m_data[i]);
                    break;
                case Source_GamepadAxis:
                    label = tr("Gamepad Axis %1 %2").arg(m_data[i]).arg(m_extra[i] ? "+" : "-");
                    break;
                case Source_JoystickButton:
                    label = tr("Joystick Button %1").arg(m_data[i]);
                    break;
                case Source_JoystickAxis:
                    label = tr("Joystick Axis %1 %2").arg(m_data[i]).arg(m_extra[i] ? "+" : "-");
                    break;
                case Source_JoystickHat:
                    label = tr("Joystick Hat %1").arg(m_data[i]);
                    break;
                default:
                    label = tr("Unbound");
                    break;
            }
            btn->setText(label);
        }
    }
}


void PsxInputDialog::startCapture(int row)
{
    m_capturing = row;
    grabKeyboard();
    openCaptureDevices();
    if (m_pollTimer != nullptr)
    {
        m_pollTimer->start();
    }
    if (m_status != nullptr)
    {
        m_status->setText(tr("Press a key or button for %1...").arg(m_names[row]));
    }
}

void PsxInputDialog::stopCapture()
{
    if (m_pollTimer != nullptr)
    {
        m_pollTimer->stop();
    }
    closeCaptureDevices();
    releaseKeyboard();
    m_capturing = -1;
}

void PsxInputDialog::openCaptureDevices()
{
    closeCaptureDevices();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; i < count; i++)
    {
        CaptureDevice device;
        if (SDL_IsGamepad(ids[i]))
        {
            device.isGamepad = true;
            device.gamepad = SDL_OpenGamepad(ids[i]);
            if (device.gamepad == nullptr) continue;
        }
        else
        {
            device.joystick = SDL_OpenJoystick(ids[i]);
            if (device.joystick == nullptr) continue;
        }
        m_captureDevices.push_back(device);
    }
    if (ids != nullptr) SDL_free(ids);

    SDL_PumpEvents();
    SDL_UpdateJoysticks();
    for (CaptureDevice& device : m_captureDevices)
    {
        if (device.isGamepad)
        {
            const int buttons = SDL_GAMEPAD_BUTTON_COUNT;
            const int axes = SDL_GAMEPAD_AXIS_COUNT;
            device.buttonPrev.assign(buttons, false);
            device.axisPrev.assign(axes, 0);
            for (int i = 0; i < buttons; i++)
                device.buttonPrev[i] = SDL_GetGamepadButton(device.gamepad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < axes; i++)
                device.axisPrev[i] = SDL_GetGamepadAxis(device.gamepad, static_cast<SDL_GamepadAxis>(i));
        }
        else
        {
            const int buttons = SDL_GetNumJoystickButtons(device.joystick);
            const int axes = SDL_GetNumJoystickAxes(device.joystick);
            const int hats = SDL_GetNumJoystickHats(device.joystick);
            device.buttonPrev.assign(buttons, false);
            device.axisPrev.assign(axes, 0);
            device.hatPrev.assign(hats, 0);
            for (int i = 0; i < buttons; i++)
                device.buttonPrev[i] = SDL_GetJoystickButton(device.joystick, i);
            for (int i = 0; i < axes; i++)
                device.axisPrev[i] = SDL_GetJoystickAxis(device.joystick, i);
            for (int i = 0; i < hats; i++)
                device.hatPrev[i] = SDL_GetJoystickHat(device.joystick, i);
        }
    }
}

void PsxInputDialog::closeCaptureDevices()
{
    for (CaptureDevice& device : m_captureDevices)
    {
        if (device.gamepad != nullptr) { SDL_CloseGamepad(device.gamepad); device.gamepad = nullptr; }
        else if (device.joystick != nullptr) { SDL_CloseJoystick(device.joystick); device.joystick = nullptr; }
    }
    m_captureDevices.clear();
}

bool PsxInputDialog::pollCaptureDevices()
{
    SDL_PumpEvents();
    SDL_UpdateJoysticks();

    for (CaptureDevice& device : m_captureDevices)
    {
        if (device.isGamepad)
        {
            const int buttons = SDL_GAMEPAD_BUTTON_COUNT;
            for (int i = 0; i < buttons; i++)
            {
                const bool now = SDL_GetGamepadButton(device.gamepad, static_cast<SDL_GamepadButton>(i));
                const bool prev = (static_cast<size_t>(i) < device.buttonPrev.size()) ? device.buttonPrev[i] : false;
                if (now && !prev)
                {
                    m_capturedBinding.type = Source_GamepadButton;
                    m_capturedBinding.data = i;
                    m_capturedBinding.extra = 0;
                    return true;
                }
                if (static_cast<size_t>(i) < device.buttonPrev.size()) device.buttonPrev[i] = now;
            }
            const int axes = SDL_GAMEPAD_AXIS_COUNT;
            for (int i = 0; i < axes; i++)
            {
                const int now = SDL_GetGamepadAxis(device.gamepad, static_cast<SDL_GamepadAxis>(i));
                const int prev = (static_cast<size_t>(i) < device.axisPrev.size()) ? device.axisPrev[i] : 0;
                if (std::abs(now) >= 16384 && now != prev)
                {
                    m_capturedBinding.type = Source_GamepadAxis;
                    m_capturedBinding.data = i;
                    m_capturedBinding.extra = (now > 0) ? 1 : 0;
                    return true;
                }
                if (static_cast<size_t>(i) < device.axisPrev.size()) device.axisPrev[i] = now;
            }
        }
        else
        {
            const int buttons = SDL_GetNumJoystickButtons(device.joystick);
            for (int i = 0; i < buttons; i++)
            {
                const bool now = SDL_GetJoystickButton(device.joystick, i);
                const bool prev = (static_cast<size_t>(i) < device.buttonPrev.size()) ? device.buttonPrev[i] : false;
                if (now && !prev)
                {
                    m_capturedBinding.type = Source_JoystickButton;
                    m_capturedBinding.data = i;
                    m_capturedBinding.extra = 0;
                    return true;
                }
                if (static_cast<size_t>(i) < device.buttonPrev.size()) device.buttonPrev[i] = now;
            }
            const int axes = SDL_GetNumJoystickAxes(device.joystick);
            for (int i = 0; i < axes; i++)
            {
                const int now = SDL_GetJoystickAxis(device.joystick, i);
                const int prev = (static_cast<size_t>(i) < device.axisPrev.size()) ? device.axisPrev[i] : 0;
                if (std::abs(now) >= 16384 && now != prev)
                {
                    m_capturedBinding.type = Source_JoystickAxis;
                    m_capturedBinding.data = i;
                    m_capturedBinding.extra = (now > 0) ? 1 : 0;
                    return true;
                }
                if (static_cast<size_t>(i) < device.axisPrev.size()) device.axisPrev[i] = now;
            }
            const int hats = SDL_GetNumJoystickHats(device.joystick);
            for (int i = 0; i < hats; i++)
            {
                const int now = SDL_GetJoystickHat(device.joystick, i);
                const int prev = (static_cast<size_t>(i) < device.hatPrev.size()) ? device.hatPrev[i] : 0;
                if (now != prev && now != 0)
                {
                    m_capturedBinding.type = Source_JoystickHat;
                    m_capturedBinding.data = i;
                    m_capturedBinding.extra = now;
                    return true;
                }
                if (static_cast<size_t>(i) < device.hatPrev.size()) device.hatPrev[i] = now;
            }
        }
    }
    return false;
}

void PsxInputDialog::keyPressEvent(QKeyEvent* event)
{
    if (m_capturing < 0)
    {
        QDialog::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape)
    {
        stopCapture();
        if (m_status != nullptr) m_status->setText(tr("Cancelled."));
        return;
    }
    if (event->isAutoRepeat()) return;

    const int scancode = Utilities::QtKeyToSdl3Key(event->key());
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT)
    {
        m_capturedBinding.type = Source_Keyboard;
        m_capturedBinding.data = scancode;
        m_capturedBinding.extra = 0;
        // commit directly
        int idx = m_capturing;
        m_types[idx] = m_capturedBinding.type;
        m_data[idx] = m_capturedBinding.data;
        m_extra[idx] = m_capturedBinding.extra;
        stopCapture();
        refreshTable();
        if (m_status != nullptr) m_status->setText(tr("Bound. Click 'Bind' for another button."));
    }
}

void PsxInputDialog::closeEvent(QCloseEvent* event)
{
    stopCapture();
    QDialog::closeEvent(event);
}

} // namespace Dialog
} // namespace UserInterface
