extends Node3D
class_name CaveSystemGenerator

## Procedural cave-tunnel generator for Godot 4.x.
## Generates a branching network of organic worm-tunnels, extrudes them into
## tube meshes, and builds trimesh collision.
##
## IMPORTANT: Attach this script to its OWN Node3D, separate from your
## Camera3D / DirectionalLight3D. This node clears and rebuilds its own
## children every time it generates, so anything parented under it
## (including a camera or light, if you put one here by mistake) will get
## deleted. Keep your camera/light as siblings under the scene root instead.
##
## v3: fixed _clear_generated_nodes() deleting ALL children (including a
## camera/light if present) instead of only its own generated geometry.
## Cleanup is now name-tagged and selective. Added optional camera autofit.

@export_group("Generation Settings")
@export var use_random_seed: bool = true
@export var random_seed: int = 0
@export var num_primary_tunnels: int = 3
@export var min_segments: int = 40
@export var max_segments: int = 70
@export var segment_length: float = 3.0
@export_range(0.0, 1.0) var turn_noise_strength: float = 0.6
@export var min_radius: float = 1.5
@export var max_radius: float = 3.0

@export_group("Chambers")
@export_range(0.0, 1.0) var chamber_chance: float = 0.04
@export var chamber_radius_multiplier: float = 2.5
@export var chamber_falloff_segments: int = 3 # how many neighboring points blend toward the chamber size

@export_group("Branching")
@export_range(0.0, 1.0) var branch_chance_per_segment: float = 0.02
@export var max_branches_per_tree: int = 6
@export_range(0.0, 1.0) var branch_min_parent_progress: float = 0.2
@export var max_branch_depth: int = 3

@export_group("Mesh")
@export var ring_sides: int = 10
@export var generate_collision: bool = true

@export_group("Debug")
@export var debug_logging: bool = true
@export var debug_draw_path_points: bool = false # spawns small spheres at every path point - expensive, use for one-off inspection only
@export var debug_autofit_camera: bool = false # moves the active Camera3D to frame the generated geometry - handy while iterating, turn off once your level has its own camera rig

# Names used to tag generated nodes, so cleanup only ever removes nodes this
# script created - never a camera, light, or anything else placed under it.
const TUNNEL_MESH_NAME := "TunnelMesh"
const TUNNEL_COLLISION_NAME := "TunnelCollision"
const DEBUG_POINTS_NAME := "DebugPathPoints"
const _GENERATED_NAMES := [TUNNEL_MESH_NAME, TUNNEL_COLLISION_NAME, DEBUG_POINTS_NAME]

var _rng := RandomNumberGenerator.new()
var _noise := FastNoiseLite.new()

class TunnelPath:
	var points: Array[Vector3] = []
	var radii: Array[float] = []

var paths: Array[TunnelPath] = []
var exit_points: Array[Vector3] = [] # end of every leaf tunnel - candidates for level exits / chamber placement


func _ready() -> void:
	generate_caves()


func generate_caves() -> void:
	var start_time := Time.get_ticks_msec()

	_rng.seed = randi() if use_random_seed else random_seed
	_noise.seed = _rng.seed
	_noise.frequency = 0.15
	_noise.noise_type = FastNoiseLite.TYPE_SIMPLEX

	_debug_log("=== generate_caves() start (seed: %d) ===" % _rng.seed)

	paths.clear()
	exit_points.clear()
	_clear_generated_nodes()

	for i in num_primary_tunnels:
		var start_pos := Vector3.ZERO
		if i > 0:
			start_pos = Vector3(_rng.randf_range(-6, 6), _rng.randf_range(-3, 1), _rng.randf_range(-6, 6))
		var start_dir := Vector3(
			_rng.randf_range(-1, 1),
			_rng.randf_range(-0.25, 0.25), # keep tunnels roughly horizontal-biased; tweak for verticality
			_rng.randf_range(-1, 1)
		).normalized()

		_debug_log("Starting primary tunnel %d at %s, dir %s" % [i, start_pos, start_dir])
		_grow_tunnel(start_pos, start_dir, _rng.randi_range(min_segments, max_segments), max_branches_per_tree, 0, i)

	_debug_log("Walk phase complete: %d total paths, %d exit points" % [paths.size(), exit_points.size()])
	for i in paths.size():
		var p := paths[i]
		if p.points.is_empty():
			_debug_log("  [WARNING] path %d has zero points" % i)
		else:
			_debug_log("  path %d: %d points, starts %s, ends %s" % [i, p.points.size(), p.points[0], p.points[p.points.size() - 1]])

	_build_all_meshes()

	if debug_draw_path_points:
		_debug_draw_points()

	if debug_autofit_camera:
		_debug_autofit_camera()

	var elapsed := Time.get_ticks_msec() - start_time
	_debug_log("=== generate_caves() complete in %d ms ===" % elapsed)


## Only removes nodes THIS script created (matched by name tag). Never
## touches a camera, light, or any other node someone parented here -
## this was the bug in v2: get_children() with no filter deleted everything,
## including a Camera3D/DirectionalLight3D placed under the same Node3D.
func _clear_generated_nodes() -> void:
	var removed := 0
	for child in get_children():
		if child.name in _GENERATED_NAMES:
			child.queue_free()
			removed += 1
	if removed > 0:
		_debug_log("Cleared %d previously generated child nodes" % removed)


## Recursively walks a tunnel path, occasionally spawning branch tunnels.
func _grow_tunnel(start_pos: Vector3, start_dir: Vector3, segment_count: int, branches_left: int, depth: int, walk_index: int) -> void:
	if depth > max_branch_depth:
		return

	var path := TunnelPath.new()
	var pos := start_pos
	var dir := start_dir.normalized()
	path.points.append(pos)
	path.radii.append(_rng.randf_range(min_radius, max_radius))

	# Unique noise offset per walk so parallel tunnels don't curve identically.
	var noise_offset := Vector3(
		_rng.randf_range(0, 10000),
		_rng.randf_range(0, 10000),
		_rng.randf_range(0, 10000)
	)

	var branches_spawned := 0

	for s in segment_count:
		var t := float(s) * 0.12
		var turn := Vector3(
			_noise.get_noise_3d(noise_offset.x + t, 0.0, 0.0),
			_noise.get_noise_3d(0.0, noise_offset.y + t, 0.0) * 0.5, # dampen vertical steering
			_noise.get_noise_3d(0.0, 0.0, noise_offset.z + t)
		) * turn_noise_strength

		dir = (dir + turn * 0.3).normalized()
		pos += dir * segment_length

		var radius := _rng.randf_range(min_radius, max_radius)
		if _rng.randf() < chamber_chance:
			radius *= chamber_radius_multiplier
			_blend_chamber_into_neighbors(path, radius)

		path.points.append(pos)
		path.radii.append(radius)

		# Chance to spawn a branch tunnel from this point.
		if branches_left > 0 and s > segment_count * branch_min_parent_progress and _rng.randf() < branch_chance_per_segment:
			branches_left -= 1
			branches_spawned += 1
			var branch_dir := dir.rotated(Vector3.UP, _rng.randf_range(-PI * 0.6, PI * 0.6))
			var side_axis := dir.cross(Vector3.UP)
			if side_axis.length() > 0.001:
				branch_dir = branch_dir.rotated(side_axis.normalized(), _rng.randf_range(-0.4, 0.4))
			_grow_tunnel(pos, branch_dir, int(segment_count * 0.6), branches_left, depth + 1, walk_index)

	if branches_spawned > 0:
		_debug_log("  walk %d (depth %d) spawned %d branches" % [walk_index, depth, branches_spawned])

	exit_points.append(pos)
	paths.append(path)


## Softens a sudden chamber radius spike into the last few points so the
## transition reads as a widening cavern instead of a jarring balloon.
func _blend_chamber_into_neighbors(path: TunnelPath, chamber_radius: float) -> void:
	var count := path.radii.size()
	var loop_count: int = min(chamber_falloff_segments, count)
	for i in loop_count:
		var idx: int = count - 1 - i
		var blend: float = 1.0 - (float(i) / chamber_falloff_segments)
		path.radii[idx] = lerp(path.radii[idx], chamber_radius, blend * 0.5)


func _build_all_meshes() -> void:
	var built := 0
	var skipped := 0
	for path in paths:
		if path.points.size() < 2:
			skipped += 1
			continue
		_build_tunnel_mesh(path)
		built += 1
	_debug_log("Mesh phase complete: %d meshes built, %d paths skipped (too few points)" % [built, skipped])


## Extrudes a path into a tube mesh using a parallel-transport frame (carries
## the "up" vector forward from ring to ring) so the tube doesn't twist as it
## turns. Winding is set so normals face INWARD, since players are inside
## the tunnel looking at the walls, not outside looking at a worm.
## UVs are generated per-vertex (U around the tube circumference, V along its
## length) - required for generate_tangents() to work, and useful later for
## texturing/triplanar shaders on the cave walls.
func _build_tunnel_mesh(path: TunnelPath) -> void:
	if path.points.size() < 2:
		return

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	var rings: Array = []
	var up := Vector3.UP

	for i in path.points.size():
		var pos: Vector3 = path.points[i]
		var tangent: Vector3
		if i == 0:
			tangent = (path.points[i + 1] - pos).normalized()
		elif i == path.points.size() - 1:
			tangent = (pos - path.points[i - 1]).normalized()
		else:
			tangent = (path.points[i + 1] - path.points[i - 1]).normalized()

		var reference_up := up
		if absf(tangent.dot(reference_up)) > 0.99:
			reference_up = Vector3.RIGHT
		var right := tangent.cross(reference_up).normalized()
		var new_up := right.cross(tangent).normalized()
		up = new_up # carried forward - this is the "parallel transport" step

		var ring_verts: Array[Vector3] = []
		for s in ring_sides:
			var angle := (float(s) / ring_sides) * TAU
			var offset := (right * cos(angle) + up * sin(angle)) * path.radii[i]
			ring_verts.append(pos + offset)
		rings.append(ring_verts)

	var vertex_count := 0
	for i in rings.size() - 1:
		var ring_a: Array = rings[i]
		var ring_b: Array = rings[i + 1]
		var v0 := float(i) / (rings.size() - 1)
		var v1 := float(i + 1) / (rings.size() - 1)

		for s in ring_sides:
			var s_next := (s + 1) % ring_sides
			var u0 := float(s) / ring_sides
			var u1 := float(s_next) / ring_sides
			var a0: Vector3 = ring_a[s]
			var a1: Vector3 = ring_a[s_next]
			var b0: Vector3 = ring_b[s]
			var b1: Vector3 = ring_b[s_next]

			# Wound for inward-facing normals. If your tunnel renders
			# inside-out (walls invisible from inside), swap the winding
			# order on these two triangles (a0/a1/b0/b1 order below).
			st.set_uv(Vector2(u0, v0))
			st.add_vertex(a0)
			st.set_uv(Vector2(u0, v1))
			st.add_vertex(b0)
			st.set_uv(Vector2(u1, v0))
			st.add_vertex(a1)

			st.set_uv(Vector2(u1, v0))
			st.add_vertex(a1)
			st.set_uv(Vector2(u0, v1))
			st.add_vertex(b0)
			st.set_uv(Vector2(u1, v1))
			st.add_vertex(b1)

			vertex_count += 6

	st.generate_normals()
	st.generate_tangents()
	var mesh := st.commit()

	_debug_log("  built tunnel mesh: %d rings, %d verts" % [rings.size(), vertex_count])

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = TUNNEL_MESH_NAME
	mesh_instance.mesh = mesh
	add_child(mesh_instance)
	mesh_instance.owner = get_tree().edited_scene_root if Engine.is_editor_hint() else null

	if generate_collision:
		var body := StaticBody3D.new()
		body.name = TUNNEL_COLLISION_NAME
		var shape := CollisionShape3D.new()
		shape.shape = mesh.create_trimesh_shape()
		body.add_child(shape)
		add_child(body)


## Spawns a small sphere at every generated path point - useful for a single
## debug run to visually confirm the walk shape even if the tube mesh itself
## isn't rendering correctly yet. Expensive with large tunnel counts; leave
## debug_draw_path_points off once the mesh is confirmed working.
func _debug_draw_points() -> void:
	var debug_root := Node3D.new()
	debug_root.name = DEBUG_POINTS_NAME
	add_child(debug_root)

	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = 0.15
	sphere_mesh.height = 0.3

	var count := 0
	for path in paths:
		for point in path.points:
			var marker := MeshInstance3D.new()
			marker.mesh = sphere_mesh
			marker.position = point
			debug_root.add_child(marker)
			count += 1

	_debug_log("Spawned %d debug path point markers" % count)


## Moves the active Camera3D to frame the full generated cave network,
## computed from the actual bounding box of every path point. Handy while
## iterating so you never have to manually hunt for the geometry - turn
## this off once you're placing your camera deliberately (e.g. a player
## rig) instead of just trying to see what got generated.
func _debug_autofit_camera() -> void:
	if paths.is_empty():
		_debug_log("[WARNING] Autofit skipped - no paths generated.")
		return

	var bounds := AABB(paths[0].points[0], Vector3.ZERO)
	for path in paths:
		for pt in path.points:
			bounds = bounds.expand(pt)

	var center := bounds.get_center()
	var extent := bounds.size.length()

	var cam := get_viewport().get_camera_3d()
	if cam == null:
		_debug_log("[WARNING] No active Camera3D found in viewport - can't autofit. Make sure a Camera3D exists as a sibling (not a child of the generator) and current is true.")
		return

	var distance := max(extent * 0.75, 10.0)
	cam.global_position = center + Vector3(0, extent * 0.25, distance)
	cam.look_at(center, Vector3.UP)
	_debug_log("Autofit camera: bounds center %s, extent %.1f, camera moved to %s" % [center, extent, cam.global_position])


func _debug_log(message: String) -> void:
	if debug_logging:
		print("[CaveGen] ", message)


## Utility: returns the leaf-end points of every tunnel walk - good
## candidates for level exits, loot chambers, or threat spawn zones.
func get_exit_points() -> Array[Vector3]:
	return exit_points
