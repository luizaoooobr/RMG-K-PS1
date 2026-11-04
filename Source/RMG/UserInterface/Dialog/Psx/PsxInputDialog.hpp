#ifndef PSXINPUTDIALOG_HPP
#define PSXINPUTDIALOG_HPP

#include <QDialog>
#include <QTimer>
#include <QString>
#include <vector>
#include <SDL3/SDL.h>

class QTableWidget;
class QPushButton;
class QLabel;
class QKeyEvent;

namespace UserInterface
{
namespace Dialog
{

// PlayStation input remapper using the same capture mechanism as the arcade
// dialog: keyboard, gamepad and joystick, with proper baseline seeding.
class PsxInputDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PsxInputDialog(QWidget* parent = nullptr);
    ~PsxInputDialog() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct CaptureDevice
    {
        bool isGamepad = false;
        SDL_Gamepad* gamepad = nullptr;
        SDL_Joystick* joystick = nullptr;
        SDL_JoystickID deviceId = 0;
        std::vector<bool> buttonPrev;
        std::vector<int> axisPrev;
        std::vector<int> hatPrev;
    };

    struct Binding
    {
        int type  = 0;
        int data  = 0;
        int extra = 0;
    };

    void loadBindings();
    void saveBindings();
    void refreshTable();
    void startCapture(int row);
    void stopCapture();
    void openCaptureDevices();
    void closeCaptureDevices();
    bool pollCaptureDevices();

    QTableWidget* m_table = nullptr;
    QLabel*      m_status = nullptr;
    QTimer*      m_pollTimer = nullptr;
    int          m_capturing = -1;

    std::vector<QString> m_names;
    std::vector<int>     m_types;   // 0=keyboard, 1=gamepad button, 2=gamepad axis
    std::vector<int>     m_data;
    std::vector<int>     m_extra;

    std::vector<CaptureDevice> m_captureDevices;

    Binding m_capturedBinding;

    int m_deviceType[4];
};

} // namespace Dialog
} // namespace UserInterface

#endif // PSXINPUTDIALOG_HPP
