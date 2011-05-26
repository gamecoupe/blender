# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>
import bpy
from bpy.props import *


class CONSOLE_HT_header(bpy.types.Header):
    bl_space_type = 'CONSOLE'

    def draw(self, context):
        sc = context.space_data
        # text = sc.text
        layout = self.layout

        row = layout.row(align=True)
        row.template_header()

        if context.area.show_menus:
            sub = row.row(align=True)

            if sc.console_type == 'REPORT':
                sub.menu("CONSOLE_MT_report")
            else:
                sub.menu("CONSOLE_MT_console")

        layout.separator()
        layout.prop(sc, "console_type", expand=True)

        if sc.console_type == 'REPORT':
            row = layout.row(align=True)
            row.prop(sc, "show_report_debug", text="Debug")
            row.prop(sc, "show_report_info", text="Info")
            row.prop(sc, "show_report_operator", text="Operators")
            row.prop(sc, "show_report_warning", text="Warnings")
            row.prop(sc, "show_report_error", text="Errors")

            row = layout.row()
            row.enabled = sc.show_report_operator
            row.operator("console.report_replay")
        else:
            row = layout.row(align=True)
            row.operator("console.autocomplete", text="Autocomplete")


class CONSOLE_MT_console(bpy.types.Menu):
    bl_label = "Console"

    def draw(self, context):
        layout = self.layout
        layout.column()
        layout.operator("console.clear")
        layout.operator("console.copy")
        layout.operator("console.paste")
        layout.menu("CONSOLE_MT_language")

        layout.separator()

        layout.operator("screen.area_dupli")
        layout.operator("screen.screen_full_area")


class CONSOLE_MT_report(bpy.types.Menu):
    bl_label = "Report"

    def draw(self, context):
        layout = self.layout
        layout.column()
        layout.operator("console.select_all_toggle")
        layout.operator("console.select_border")
        layout.operator("console.report_delete")
        layout.operator("console.report_copy")


class CONSOLE_MT_language(bpy.types.Menu):
    bl_label = "Languages..."

    def draw(self, context):
        import sys

        layout = self.layout
        layout.column()

        # Collect modules with 'console_*.execute'
        languages = []
        for modname, mod in sys.modules.items():
            if modname.startswith("console_") and hasattr(mod, "execute"):
                languages.append(modname.split('_', 1)[-1])

        languages.sort()

        for language in languages:
            layout.operator("console.language", text=language[0].upper() + language[1:]).language = language


def add_scrollback(text, text_type):
    for l in text.split('\n'):
        bpy.ops.console.scrollback_append(text=l.replace('\t', '    '),
            type=text_type)


class ConsoleExec(bpy.types.Operator):
    '''Execute the current console line as a python expression'''
    bl_idname = "console.execute"
    bl_label = "Console Execute"

    def execute(self, context):
        sc = context.space_data

        module = __import__("console_" + sc.language)
        execute = getattr(module, "execute", None)

        if execute:
            return execute(context)
        else:
            print("Error: bpy.ops.console.execute_" + sc.language + " - not found")
            return {'FINISHED'}


class ConsoleAutocomplete(bpy.types.Operator):
    '''Evaluate the namespace up until the cursor and give a list of options or complete the name if there is only one'''
    bl_idname = "console.autocomplete"
    bl_label = "Console Autocomplete"

    @classmethod
    def poll(cls, context):
        return context.space_data.console_type != 'REPORT'

    def execute(self, context):
        sc = context.space_data
        module = __import__("console_" + sc.language)
        autocomplete = getattr(module, "autocomplete", None)

        if autocomplete:
            return autocomplete(context)
        else:
            print("Error: bpy.ops.console.autocomplete_" + sc.language + " - not found")
            return {'FINISHED'}


class ConsoleBanner(bpy.types.Operator):
    '''Print a message whem the terminal initializes'''
    bl_idname = "console.banner"
    bl_label = "Console Banner"

    def execute(self, context):
        sc = context.space_data

        # default to python
        if not sc.language:
            sc.language = 'python'

        module = __import__("console_" + sc.language)
        banner = getattr(module, "banner", None)

        if banner:
            return banner(context)
        else:
            print("Error: bpy.ops.console.banner_" + sc.language + " - not found")
            return {'FINISHED'}


class ConsoleLanguage(bpy.types.Operator):
    '''Set the current language for this console'''
    bl_idname = "console.language"
    bl_label = "Console Language"
    language = StringProperty(name="Language", maxlen=32, default="")

    def execute(self, context):
        sc = context.space_data

        # defailt to python
        sc.language = self.language

        bpy.ops.console.banner()

        # insert a new blank line
        bpy.ops.console.history_append(text="", current_character=0,
            remove_duplicates=True)

        return {'FINISHED'}


def register():
    bpy.utils.register_module(__name__)


def unregister():
    bpy.utils.unregister_module(__name__)

if __name__ == "__main__":
    register()
