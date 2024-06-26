"""
Extending the Button Context Menu
+++++++++++++++++++++++++++++++++

This example enables you to insert your own menu entry into the common
right click menu that you get while hovering over a UI button (e.g. operator,
value field, color, string, etc.)

To make the example work, you have to first select an object
then right click on an user interface element (maybe a color in the
material properties) and choose *Execute Custom Action*.

Executing the operator will then print all values.
"""

import bpy


def dump(obj, text):
    for attr in dir(obj):
        print("{!r}.{:s} = {:s}".format(obj, attr, getattr(obj, attr)))


class WM_OT_button_context_test(bpy.types.Operator):
    """Right click entry test"""
    bl_idname = "wm.button_context_test"
    bl_label = "Run Context Test"

    @classmethod
    def poll(cls, context):
        return context.active_object is not None

    def execute(self, context):
        value = getattr(context, "button_pointer", None)
        if value is not None:
            dump(value, "button_pointer")

        value = getattr(context, "button_prop", None)
        if value is not None:
            dump(value, "button_prop")

        value = getattr(context, "button_operator", None)
        if value is not None:
            dump(value, "button_operator")

        return {'FINISHED'}


def draw_menu(self, context):
    layout = self.layout
    layout.separator()
    layout.operator(WM_OT_button_context_test.bl_idname)


def register():
    bpy.utils.register_class(WM_OT_button_context_test)
    bpy.types.UI_MT_button_context_menu.append(draw_menu)


def unregister():
    bpy.types.UI_MT_button_context_menu.remove(draw_menu)
    bpy.utils.unregister_class(WM_OT_button_context_test)


if __name__ == "__main__":
    register()
