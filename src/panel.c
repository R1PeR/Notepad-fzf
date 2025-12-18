#include "panel.h"

Rectangle DrawPanel(int posX, int posY, int width, int height)
{
    // Draw overlay to dim background
    // DrawRectangle(posX, posY, width, height, (Color){ 0, 0, 0, 180 });

    Rectangle panelRect = { posX + 100, posY + 100, width - 200, height - 200 };

    DrawRectangle(panelRect.x, panelRect.y, panelRect.width, panelRect.height, (Color){ 35, 35, 35, 255 });
    DrawRectangleLines(panelRect.x, panelRect.y, panelRect.width, panelRect.height, (Color){ 100, 100, 100, 255 });
    return panelRect;
}
