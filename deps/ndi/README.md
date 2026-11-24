# NDI® SDK headers (deps/ndi)

NDI® is a registered trademark of Vizrt NDI AB. More about NDI: <https://ndi.video/>.

The C headers from the NDI SDK 6.4.0.2 (`Version.txt`), unchanged apart from line endings (LF). They are used by `plugins/ctd-ndi` to load the NDI
library at run time; nothing links against it.

Each header carries its own MIT licence notice ("The following MIT license applies to this file ONLY and not to the SDK
as a whole"). The NDI SDK documentation (§4.1) allows open-source projects to include them: "Note that open-source
projects have the right to include the header files within their distributions, which may then be used with dynamic
loading of the NDI libraries."

The NDI library itself is **not** in this repository and isn't shipped with CTD OBS: see `docs/ctd/ndi.md`.
