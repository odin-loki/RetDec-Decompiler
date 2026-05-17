#include <QApplication>
#include <QLabel>
#include <QMainWindow>

#include <cstring>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            return 0;
        }
    }

    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("retdec-gui (stub)");
    window.setMinimumSize(640, 480);

    auto *label = new QLabel("retdec-gui stub: Qt GUI will be implemented later.", &window);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(label);

    window.show();
    return app.exec();
}

