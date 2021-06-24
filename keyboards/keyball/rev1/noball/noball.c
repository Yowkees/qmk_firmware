/*
Copyright 2021 @Yowkees
Copyright 2021 MURAOKA Taro (aka KoRoN, @kaoriya)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include QMK_KEYBOARD_H

#include "pointing_device.h"
#include "trackball.h"

// TODO: modify matrix_mask by secondary board type (has ball or no balls)
matrix_row_t matrix_mask[MATRIX_ROWS] = {
    0b0111111,
    0b0111111,
    0b0011111,
    0b0111111,

    0b0111111,
    0b0111111,
    0b0011111,
    0b0111111,
};

bool trackball_has(void) {
    // rev1/ball has a trackball always.
    return false;
}

void trackball_process_secondary_kb(int8_t dx, int8_t dy) {
    trackball_process_user(dx, dy);
    pointing_device_send();
}
