PHP_ARG_ENABLE([phpatcher],
  [whether to enable phpatcher support],
  [AS_HELP_STRING([--enable-phpatcher],
    [Enable phpatcher: in-memory phpatcher-ed patching of PHP sources])],
  [no])

if test "$PHP_PHPATCHER" != "no"; then
  dnl Build as C++17.
  PHP_REQUIRE_CXX()

  PHP_ADD_BUILD_DIR([$ext_builddir])

  CXXFLAGS="$CXXFLAGS -std=c++17"

  PHP_SUBST(PHPATCHER_SHARED_LIBADD)

  dnl The trailing "cxx" arg tells the build system these are C++ sources.
  PHP_NEW_EXTENSION(phpatcher, phpatcher.cpp patch.cpp, $ext_shared, , -std=c++17, cxx)
fi
