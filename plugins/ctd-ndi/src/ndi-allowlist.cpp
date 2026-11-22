/******************************************************************************
    CTD OBS NDI module: the NDI libraries CTD OBS accepts (docs/ctd/ndi.md,
    "The allow-list").

    Each entry is the SHA-256 of the library file inside the official NDI 6
    Runtime installer, as Vizrt ships it. A file whose hash isn't here is
    never loaded. Add an entry only after the soak test passed with that
    version; keep old entries so machines already set up keep working.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-logic.hpp"

namespace ctd_ndi {

const std::vector<AllowEntry> &AllowList()
{
	static const std::vector<AllowEntry> list = {
		/* NDI 6.4.0.2 (2026-11-17), libndi.dylib (universal arm64 + x86_64, signed by NewTek) from
		 * libNDI_for_Mac.pkg (ndi.link/NDIRedistV6Apple) */
		{Platform::MacOS, "6.4.0.2", "b10315f9435c59a5db76dd44d27ea809432468151aea458359129f279ca6f09c"},
		/* NDI 6.4.0.2 (2026-11-17), Processing.NDI.Lib.x64.dll from NDI 6 Runtime.exe (ndi.link/NDIRedistV6) */
		{Platform::Windows, "6.4.0.2", "adca7e9c98ddddb01a1b684f90c97d0bd764990ee2839524fde51ab37f21028a"},
	};
	return list;
}

} // namespace ctd_ndi
