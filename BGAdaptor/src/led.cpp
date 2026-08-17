#include "led.h"
#include "state.h"
#include "transport.h"

void updateLEDStatus() {
    pixels.clear();
    if (WiFi.status() != WL_CONNECTED) {
        pixels.setPixelColor(0, pixels.Color(0, 2, 0));  // Red for WiFi issue
    } else if (!productConnected()) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 2));  // Blue for product connection issue
    } else {
        pixels.setPixelColor(0, pixels.Color(2, 0, 0));  // Green for everything else
    }
    pixels.show();
}
