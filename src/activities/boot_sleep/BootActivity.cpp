#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/Faraday200.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Faraday200, (pageWidth - 200) / 2, (pageHeight - 200) / 2, 200, 200);
  // Clear of the art: at 200px the image reaches pageHeight/2 + 100.
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 120, tr(STR_FARADAY), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 145, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
