#include "register_types.h"

#include "avatar_camera_ring.h"
#include "avatar_live_pipeline.h"
#include "avatar_live_session.h"
#include "avatar_model.h"
#include "avatar_output_manager.h"
#include "avatar_runtime.h"
#include "avatar_single_instance.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_avatar_studio_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    GDREGISTER_CLASS(AvatarRuntime);
    GDREGISTER_CLASS(AvatarModel);
    GDREGISTER_CLASS(AvatarOutputManager);
    GDREGISTER_CLASS(AvatarLiveSession);
    GDREGISTER_CLASS(AvatarLivePipeline);
    GDREGISTER_CLASS(AvatarCameraRing);
    GDREGISTER_CLASS(AvatarSingleInstance);
}

void uninitialize_avatar_studio_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {
// Entry point named by app/bin/avatar_studio.gdextension.
GDExtensionBool GDE_EXPORT avatar_studio_library_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization* r_initialization) {
    ::godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library,
                                                     r_initialization);

    init_obj.register_initializer(initialize_avatar_studio_module);
    init_obj.register_terminator(uninitialize_avatar_studio_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

    return init_obj.init();
}
}
