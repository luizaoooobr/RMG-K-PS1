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

extern void FbneoLibretroInputReloadSettings(int profileId);
#include "Utilities/QtKeyToSdl3Key.hpp"

#include <SDL3/SDL.h>
#include <cmath>

namespace UserInterface
{
namespace Dialog
{

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

PsxInputDialog::PsxInputDialog(LibretroInputProfileId profile, QWidget* parent)
    : QDialog(parent),
      m_profileId(profile),
      m_profile(&libretroInputProfile(profile))
{
    const int count = m_profile->count;
    setWindowTitle(tr(m_profile->title));
    setModal(true);
    resize(520, profile == LibretroInputProfileId::Psx ? 480 :
                 (profile == LibretroInputProfileId::Ps2 ? 680 : 560));

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
        m_deviceType[p] = UserInterface::Dialog::loadProfileDeviceType(*m_profile, p, 0);
    }

    deviceCombo->setCurrentIndex(deviceCombo->findData(m_deviceType[0]));
    connect(playerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, deviceCombo](int idx) {
        deviceCombo->setCurrentIndex(deviceCombo->findData(m_deviceType[idx]));
    });
    connect(deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, playerCombo, deviceCombo](int) {
        m_deviceType[playerCombo->currentIndex()] = deviceCombo->currentData().toInt();
    });

    // Table
    m_table = new QTableWidget(count, 2, this);
    m_table->setHorizontalHeaderLabels({tr("Button"), tr("Binding")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int i = 0; i < count; i++)
    {
        m_table->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(m_profile->names[i])));
        auto* btn = new QPushButton(tr("Bind"), this);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QPushButton::clicked, this, [this, i]() { this->startCapture(i); });
        m_table->setCellWidget(i, 1, btn);
    }
    layout->addWidget(m_table);

    // Double-clicking any cell of a row starts capture, exactly like the
    // "Bind" button in that row.
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                if (row >= 0 && row < m_profile->count)
                {
                    this->startCapture(row);
                }
            });

    m_status = new QLabel(tr("Click 'Bind' or double-click a row, then press a key or button."), this);
    layout->addWidget(m_status);

    auto* saveBtn = new QPushButton(tr("Save"), this);
    connect(saveBtn, &QPushButton::clicked, this, [this]() { this->saveBindings(); this->accept(); });
    layout->addWidget(saveBtn);

    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
    {
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }

    // Load existing bindings from the unified section and both legacy arcade
    // sections. Every historic name maps to the same hardware button, so a
    // saved key follows that button instead of the row number it used to have.
    std::vector<std::string> savedNames;
    std::vector<int> savedTypes, savedData, savedExtra;
    UserInterface::Dialog::loadProfileBindingSettings(*m_profile, savedNames, savedTypes,
                                                      savedData, savedExtra);
    std::vector<std::string> boundNames;
    remapLibretroSavedBindings(*m_profile, savedNames, savedTypes, savedData, savedExtra,
                               boundNames, m_types, m_data, m_extra);
    for (const std::string& name : boundNames)
    {
        m_names.push_back(QString::fromUtf8(name));
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
                m_status->setText(tr("Bound. Click 'Bind' or double-click another row."));
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

    const std::string section = m_profile->bindingSection;
    CoreSettingsSetValue(SettingsID::FBNeo_BindingNames, section, names);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingTypes, section, types);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingData, section, data);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingExtraData, section, extra);

    for (int p = 0; p < 4; p++)
    {
        const std::string deviceSection =
            std::string(m_profile->deviceSectionPrefix) + std::to_string(p);
        CoreSettingsSetValue(SettingsID::FBNeo_DeviceType, deviceSection, m_deviceType[p]);
    }

    CoreSettingsSave();

    FbneoLibretroInputReloadSettings(static_cast<int>(m_profileId));
}

void PsxInputDialog::refreshTable()
{
    for (int i = 0; i < m_profile->count; i++)
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
                    label = m_extra[i] == 2
                        ? tr("Gamepad Axis %1 (both)").arg(m_data[i])
                        : tr("Gamepad Axis %1 %2").arg(m_data[i]).arg(m_extra[i] ? "+" : "-");
                    break;
                case Source_JoystickButton:
                    label = tr("Joystick Button %1").arg(m_data[i]);
                    break;
                case Source_JoystickAxis:
                    label = m_extra[i] == 2
                        ? tr("Joystick Axis %1 (both)").arg(m_data[i])
                        : tr("Joystick Axis %1 %2").arg(m_data[i]).arg(m_extra[i] ? "+" : "-");
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
    if (row < 0 || row >= m_profile->count || m_capturing == row)
    {
        return;
    }
    if (m_capturing >= 0)
    {
        stopCapture();
    }
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
                    // Analog stick rows own the whole axis; digital rows keep
                    // the +/- half-axis encoding.
                    const bool fullAxis = m_capturing >= 0 && m_capturing < m_profile->count &&
                        isLibretroAnalogRow(m_profile->bits[m_capturing]);
                    m_capturedBinding.extra = fullAxis ? 2 : ((now > 0) ? 1 : 0);
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
                    const bool fullAxis = m_capturing >= 0 && m_capturing < m_profile->count &&
                        isLibretroAnalogRow(m_profile->bits[m_capturing]);
                    m_capturedBinding.extra = fullAxis ? 2 : ((now > 0) ? 1 : 0);
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
        if (m_status != nullptr) m_status->setText(tr("Bound. Click 'Bind' or double-click another row."));
    }
}

void PsxInputDialog::closeEvent(QCloseEvent* event)
{
    stopCapture();
    QDialog::closeEvent(event);
}

} // namespace Dialog
} // namespace UserInterface
