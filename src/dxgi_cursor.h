/*
 * This file is part of txproto.
 *
 * txproto is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * txproto is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with txproto; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <dxgi1_2.h>

typedef struct DxgiCursorHandler DxgiCursorHandler;

int sp_dxgi_cursor_handler_init(DxgiCursorHandler **ctx, uint32_t identifier);
void sp_dxgi_cursor_handler_uninit(DxgiCursorHandler **ctx);

int sp_dxgi_cursor_handler_send(DxgiCursorHandler *ctx,
                                IDXGIOutputDuplication *output_duplication,
                                const DXGI_OUTDUPL_FRAME_INFO *frame_info);
