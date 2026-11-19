#include <Arduino.h>

#include "app.h"
#include "comm.h"
#include "messages.h"
#include "ui.h"

void setup() {
    // Relay pins first: every relay stays released and K_OUT open while the rest boots.
    app::begin();
    msg::begin();
    ui::begin();
    comm::begin();
    ui::setPage(ui::Page::Resistor);
}

void loop() {
    app::loop();
    comm::loop();
    ui::loop();
    delay(1);
}
