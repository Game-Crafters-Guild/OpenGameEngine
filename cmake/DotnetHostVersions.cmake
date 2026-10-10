# ge_dotnet_versions_newest_first(<list_var>)
#
# Orders a list of .NET version directories (host packs, host/fxr) newest
# first, comparing the version numbers as numbers: 10.0.12 precedes 9.0.20.
# A plain list(SORT) compares text, which puts every 9.x ahead of 10.x.
function(ge_dotnet_versions_newest_first list_var)
  set(_versions ${${list_var}})
  list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
  set(${list_var} ${_versions} PARENT_SCOPE)
endfunction()
