# The compiler SDK has no GUI and does not ship translated UI resources.
# FreeCAD 1.1.3 calls qt_add_translation in Base even when BUILD_GUI=OFF,
# although its SetupQt.cmake only finds LinguistTools when BUILD_GUI=ON.
function(qt_add_translation output)
    set("${output}" "" PARENT_SCOPE)
endfunction()
