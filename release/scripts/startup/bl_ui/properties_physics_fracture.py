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
from bpy.types import Panel, Menu, UIList
from bpy.app.translations import pgettext_iface as iface_


class FRACTURE_MT_presets(Menu):
    bl_label = "Fracture Presets"
    preset_subdir = "fracture"
    preset_operator = "script.execute_preset"
    draw = Menu.draw_preset

class PhysicButtonsPanel():
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"

    @classmethod
    def poll(cls, context):
        ob = context.object
        rd = context.scene.render
        return (ob and (ob.type == 'MESH' or ob.type == 'CURVE' or ob.type == 'SURFACE' or ob.type == 'FONT')) and (not rd.use_game_engine) and (context.fracture)

class FRACTURE_UL_fracture_settings(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        fl = item
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            layout.prop(fl, "name", text="", emboss=False, icon_value=icon)
        elif self.layout_type in {'GRID'}:
            layout.alignment = 'CENTER'
            layout.label(text="", icon_value=icon)

class PHYSICS_PT_fracture_settings(PhysicButtonsPanel, Panel):
    bl_label = "Fracture Settings"

    def draw(self, context):
        layout = self.layout

        md = context.fracture
        ob = context.object
        layout.prop(md, "fracture_mode")

        row = layout.row()
        row.template_list("FRACTURE_UL_fracture_settings", "", md, "fracture_settings", md.fracture_settings, "active_index", rows=2)
        layout.prop(md, "dm_group")

class PHYSICS_PT_fracture(PhysicButtonsPanel, Panel):
    bl_label = "Fracture"

    def icon(self, bool):
        if bool:
            return 'TRIA_DOWN'
        else:
            return 'TRIA_RIGHT'

    def draw(self, context):
        layout = self.layout

        md = context.fracture.fracture;
        ob = context.object

        layout.label(text="Presets:")
        sub = layout.row(align=True)
        sub.menu("FRACTURE_MT_presets", text=bpy.types.FRACTURE_MT_presets.bl_label)
        sub.operator("fracture.preset_add", text="", icon='ZOOMIN')
        sub.operator("fracture.preset_add", text="", icon='ZOOMOUT').remove_active = True

        if context.fracture.fracture_mode == 'DYNAMIC':
            layout.prop(md, "dynamic_force")
            layout.prop(md, "limit_impact")

        layout.prop(md, "frac_algorithm")
        col = layout.column(align=True)
        col.prop(md, "shard_count")
        col.prop(md, "point_seed")

        if md.frac_algorithm == 'BOOLEAN' or md.frac_algorithm == 'BISECT_FILL' or md.frac_algorithm == 'BISECT_FAST_FILL':
            layout.prop(md, "inner_material")
        if md.frac_algorithm == 'BOOLEAN_FRACTAL':
            layout.prop(md, "inner_material")
            col = layout.column(align=True)
            row = col.row(align=True)
            row.prop(md, "fractal_cuts")
            row.prop(md, "fractal_iterations")
            row = col.row(align=True)
            row.prop(md, "fractal_amount")
            row.prop(md, "physics_mesh_scale")
        row = layout.row()
        row.prop(md, "shards_to_islands")
        row.prop(md, "auto_execute")
        row.prop(md, "use_smooth")
        row = layout.row(align=True)
        row.prop(md, "splinter_axis")
        layout.prop(md, "splinter_length")

        box = layout.box()
        box.prop(context.fracture, "use_experimental", text="Advanced Fracture Settings", icon=self.icon(context.fracture.use_experimental), emboss = False)
        if context.fracture.use_experimental:
            box.label("Fracture Point Source:")
            col = box.column()
            col.prop(md, "point_source")
            if 'GREASE_PENCIL' in md.point_source:
                col.prop(md, "use_greasepencil_edges")
                col.prop(md, "grease_offset")
                col.prop(md, "grease_decimate")
                col.prop(md, "cutter_axis")
            col.prop(md, "extra_group")
            if md.frac_algorithm == 'BOOLEAN':
                col.prop(md, "cutter_group")
            col.prop(md, "use_particle_birth_coordinates")

            box.prop(md, "percentage")
            box.label("Threshold Vertex Group:")
            box.prop_search(md, "thresh_vertex_group", ob, "vertex_groups", text = "")
            box.label("Passive Vertex Group:")
            box.prop_search(md, "ground_vertex_group", ob, "vertex_groups", text = "")
            box.label("Inner Vertex Group:")
            box.prop_search(md, "inner_vertex_group", ob, "vertex_groups", text = "")

        layout.operator("object.fracture_refresh", text="Execute Fracture", icon='MOD_EXPLODE')

class PHYSICS_PT_fracture_constraint_settings(PhysicButtonsPanel, Panel):
    bl_label = "Fracture Constraint Settings"

    def draw(self, context):
        layout = self.layout

        md = context.fracture
        ob = context.object

        row = layout.row()
        row.template_list("FRACTURE_UL_fracture_settings", "", md, "constraint_settings", md.constraint_settings, "active_index", rows=2)

        col = row.column(align=True)
        col.operator("object.fracture_constraint_setting_add", icon='ZOOMIN', text="")
        col.operator("object.fracture_constraint_setting_remove", icon='ZOOMOUT', text="").all = False

class PHYSICS_PT_fracture_constraint(PhysicButtonsPanel, Panel):
    bl_label = "Fracture Constraints"

    def draw(self, context):
        layout = self.layout
        md = context.fracture.constraint
        ob = context.object

        layout.label("Constraint Building Settings")
        row = layout.row()
        row.prop(md, "use_constraints")
        row.prop(md, "use_breaking")
        layout.prop(md, "constraint_target")
        col = layout.column(align=True)
        col.prop(md, "constraint_limit", text="Constraint limit, per MeshIsland")
        col.prop(md, "contact_dist")

        layout.prop(md, "cluster_count")
        layout.prop(md, "cluster_group")
        layout.prop(md, "cluster_constraint_type")

        layout.label("Constraint Breaking Settings")
        col = layout.column(align=True)
        col.prop(md, "breaking_threshold", text="Threshold")
        col.prop(md, "cluster_breaking_threshold")

        layout.label("Constraint Special Breaking Settings")
        col = layout.column(align=True)
        row = col.row(align=True)
        row.prop(md, "breaking_percentage", text="Percentage")
        row.prop(md, "cluster_breaking_percentage", text="Cluster Percentage")

        row = col.row(align=True)
        row.prop(md, "breaking_angle", text="Angle")
        row.prop(md, "cluster_breaking_angle", text="Cluster Angle")

        row = col.row(align=True)
        row.prop(md, "breaking_distance", text="Distance")
        row.prop(md, "cluster_breaking_distance", text="Cluster Distance")

        row = col.row(align=True)
        row.prop(md, "breaking_percentage_weighted")
        row.prop(md, "breaking_angle_weighted")
        row.prop(md, "breaking_distance_weighted")

        col = layout.column(align=True)
        col.prop(md, "solver_iterations_override")
        col.prop(md, "cluster_solver_iterations_override")
        layout.prop(md, "use_mass_dependent_thresholds")

class PHYSICS_PT_fracture_utilities(PhysicButtonsPanel, Panel):
    bl_label = "Fracture Utilities"

    def draw(self, context):
        layout = self.layout
        md = context.fracture.fracture
        layout.prop(md, "autohide_dist")
        row = layout.row()
        row.prop(md, "fix_normals")
        row.prop(md, "nor_range")
        if not(md.refresh):
           layout.prop(context.fracture, "execute_threaded")

        layout.operator("object.rigidbody_convert_to_objects", text = "Convert To Objects")
        layout.operator("object.rigidbody_convert_to_keyframes", text = "Convert To Keyframed Objects")

if __name__ == "__main__":  # only for live edit.
    bpy.utils.register_module(__name__)
