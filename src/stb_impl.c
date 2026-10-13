/* stb_impl.c - single translation unit for stb image I/O.
 * Headers resolve flat (vcpkg) or under stb/ (distro libstb-dev). */
#if defined(__has_include)
#  if __has_include(<stb/stb_image.h>)
#    define STB_IMAGE_IMPLEMENTATION
#    define STB_IMAGE_WRITE_IMPLEMENTATION
#    include <stb/stb_image.h>
#    include <stb/stb_image_write.h>
#  else
#    define STB_IMAGE_IMPLEMENTATION
#    define STB_IMAGE_WRITE_IMPLEMENTATION
#    include <stb_image.h>
#    include <stb_image_write.h>
#  endif
#else
#  define STB_IMAGE_IMPLEMENTATION
#  define STB_IMAGE_WRITE_IMPLEMENTATION
#  include <stb_image.h>
#  include <stb_image_write.h>
#endif
