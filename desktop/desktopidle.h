#ifndef DESKTOPIDLE_H
#define DESKTOPIDLE_H

class QWidget;

void installDesktopIdleReturn();
void setDesktopIdleReturnEnabled(bool enabled);
bool desktopFullscreenEnabled();
void showDesktopTopLevel(QWidget *window);
void showVideoTopLevel(QWidget *window);
void replaceDesktopTopLevel(QWidget *current, QWidget *next);
void showExpressionWindowAndCloseOthers();

#endif // DESKTOPIDLE_H
