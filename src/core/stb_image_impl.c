/*
 * stb_image implementation — compiled once as a separate TU.
 * Provides PNG/JPEG/BMP decoding for Liu graphics, inline images,
 * and background wallpaper loading (which needs JPEG/BMP + stdio).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "stb_image.h"
