<p align="center">
  <img src="https://github.com/user-attachments/assets/a742cb99-7423-4be7-8e2c-f12b8324fae7" width="900"/>
</p>

# Akeno-uWS
This repository is for the fork of µWebSockets.js used by [Akeno](https://github.com/the-lstv/akeno), used for building and including the µWebSockets as a module.
It moves a bunch of the JavaScript logic to C++ for slightly better performance (though it's not an enormous difference), eliminate calls to JavaScript where possible, and for the future low level cache implementation.

#### This is *not* the main repository for Akeno - please [go there instead](https://github.com/the-lstv/akeno).

## Warning
The state of this as of now is highly experimental and more of a proof-of-concept.

It is not clean or tested, which is why I recommend using the JavaScript implementations as of now, which are far more stable.

## Usage
This is designed to be used by Akeno. If you place them in this structure:
```
/akeno        # Akeno installation
/akeno-uws    # This repository
```
Akeno (1.6.8-beta and up) should automatically discover this repository and try to use it's build.
In the future this will be included directly in Akeno, but that is once it is not experimental.

## Building
First clone this repository:
```
git clone --branch master --single-branch --recursive https://github.com/the-lstv/akeno-uws.git
```
To build, simply run:
```
make
```
On Linux, this should be straightforward. Just to be safe, ensure you have the following packages installed:
- Fedora (43): `@development-tools cmake git pkgconf-pkg-config libunwind-devel`
- Ubuntu/Debian: `build-essential cmake clang-18 zlib1g-dev pkg-config libunwind-dev` (suggested by someone on GitHub)

On Windows you may need to do a bunch of extra steps based on your environment, I don't really know (in my experience, building on Windows was a pain, but I guess it may vary. You will need git, cmake, strawberry perl, likely also whatever $(CC) resolves to, and so on, and these may be more work on Windows). Note that Windows is not officially supported by Akeno so use at your own risk, I give no guarantee it runs well on Windows (just that it probably runs, maybe). I will try to help with issues, but Windows is low priority.

## Licence
Please keep in mind that this repository contains code with different licences:
Code from µWebSockets.js is licensed under the Apache License 2.0, and some code originating from Akeno authors is licensed under the GNU General Public License v3.0 (since the Akeno project itself is licensed under GPLv3).
Thus the resulting binaries are also licensed under GPLv3.