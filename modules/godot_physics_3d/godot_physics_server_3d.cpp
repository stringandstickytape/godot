/**************************************************************************/
/*  godot_physics_server_3d.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "godot_physics_server_3d.h"

#include "godot_body_direct_state_3d.h"
#include "godot_broad_phase_3d_bvh.h"
#include "joints/godot_cone_twist_joint_3d.h"
#include "joints/godot_generic_6dof_joint_3d.h"
#include "joints/godot_hinge_joint_3d.h"
#include "joints/godot_pin_joint_3d.h"
#include "joints/godot_slider_joint_3d.h"

#include "core/debugger/engine_debugger.h"
#include "core/os/memory_tracker.h"
#include "core/os/os.h"
#include "core/os/time.h"

namespace {

struct ShapeMemoryLinkCounters {
	uint64_t allocation_lookup_attempt_count = 0;
	uint64_t allocation_lookup_success_count = 0;
	uint64_t allocation_lookup_unavailable_count = 0;
	uint64_t empty_backing_count = 0;
};

String _shape_memory_address(const void *p_pointer) {
	return "0x" + String::num_uint64(uint64_t(uintptr_t(p_pointer)), 16);
}

String _shape_type_name(PhysicsServer3D::ShapeType p_type) {
	switch (p_type) {
		case PhysicsServer3D::SHAPE_WORLD_BOUNDARY:
			return "world_boundary";
		case PhysicsServer3D::SHAPE_SEPARATION_RAY:
			return "separation_ray";
		case PhysicsServer3D::SHAPE_SPHERE:
			return "sphere";
		case PhysicsServer3D::SHAPE_BOX:
			return "box";
		case PhysicsServer3D::SHAPE_CAPSULE:
			return "capsule";
		case PhysicsServer3D::SHAPE_CYLINDER:
			return "cylinder";
		case PhysicsServer3D::SHAPE_CONVEX_POLYGON:
			return "convex_polygon";
		case PhysicsServer3D::SHAPE_CONCAVE_POLYGON:
			return "concave_polygon";
		case PhysicsServer3D::SHAPE_HEIGHTMAP:
			return "heightmap";
		case PhysicsServer3D::SHAPE_SOFT_BODY:
			return "soft_body";
		case PhysicsServer3D::SHAPE_CUSTOM:
			return "custom";
	}
	return "unknown";
}

template <typename T>
Dictionary _shape_vector_cpu_backing(const Vector<T> &p_vector, bool p_tracker_inventory_complete, ShapeMemoryLinkCounters &r_counters) {
	const void *user_base = p_vector.get_memory_allocation_base();
	const void *data_address = p_vector.ptr();
	const uint64_t logical_bytes = uint64_t(p_vector.size()) * sizeof(T);
	const uint64_t capacity_bytes = uint64_t(p_vector.capacity()) * sizeof(T);
	MemoryTracker::AllocationRecord allocation;
	const bool allocation_available = user_base != nullptr && MemoryTracker::query_allocation(user_base, allocation);

	Dictionary result;
	result["tracking_enabled"] = MemoryTracker::is_enabled();
	result["tracker_start_unix_usec"] = MemoryTracker::get_tracker_start_unix_usec();
	result["tracker_inventory_complete_for_enabled_memory_api_window"] = p_tracker_inventory_complete;
	result["native_allocation_coverage_complete"] = false;
	result["has_backing"] = user_base != nullptr;
	result["allocation_available"] = allocation_available;
	result["user_base"] = _shape_memory_address(user_base);
	result["data_address"] = _shape_memory_address(data_address);
	result["logical_bytes"] = logical_bytes;
	result["container_capacity_bytes"] = capacity_bytes;
	result["container_slack_bytes"] = capacity_bytes - logical_bytes;
	result["shared_cow"] = true;
	result["cow_refcount_observed"] = uint64_t(p_vector.get_memory_refcount());

	if (user_base == nullptr) {
		r_counters.empty_backing_count++;
		result["unavailable_reason"] = "Container has no backing allocation.";
		return result;
	}

	r_counters.allocation_lookup_attempt_count++;
	if (!allocation_available) {
		r_counters.allocation_lookup_unavailable_count++;
		result["unavailable_reason"] = MemoryTracker::is_enabled() ? "Backing allocation is absent from the current tracker inventory; it may predate tracking or be excluded/incomplete. No identity inferred." : "Native allocation tracking is disabled. No identity inferred.";
		return result;
	}

	r_counters.allocation_lookup_success_count++;
	result["allocation_id"] = "memory-allocation:" + String::num_uint64(allocation.allocation_id);
	result["identity_scope"] = "process-memory-allocation-generation";
	result["requested_bytes"] = allocation.requested_bytes;
	result["raw_requested_bytes"] = allocation.raw_requested_bytes;
	result["raw_base"] = _shape_memory_address((const void *)allocation.raw_base);
	result["allocator_usable_bytes_available"] = allocation.allocator_usable_bytes_available;
	result["allocator_usable_bytes"] = allocation.allocator_usable_bytes;
	result["created_sequence"] = allocation.created_sequence;
	result["last_resize_sequence"] = allocation.last_resize_sequence;
	result["origin"] = allocation.origin == MemoryTracker::AllocationOrigin::ALLOCATION ? "allocation" : "reallocation_of_pre_enable_allocation";
	result["origin_caller_pc_available"] = allocation.origin_caller_pc != 0;
	result["origin_caller_pc"] = _shape_memory_address((const void *)allocation.origin_caller_pc);
	result["latest_successful_reallocation_caller_pc_available"] = allocation.last_reallocation_caller_pc != 0;
	result["latest_successful_reallocation_caller_pc"] = _shape_memory_address((const void *)allocation.last_reallocation_caller_pc);
	result["aligned"] = allocation.aligned;
	result["prepad"] = allocation.prepad;
	result["initial_allocation_zeroed"] = allocation.zeroed;
	result["alignment_bytes"] = allocation.alignment;
	return result;
}

template <typename T>
void _append_shape_vector_row(Array &r_rows, const String &p_shape_rid, const String &p_logical_parent_key, PhysicsServer3D::ShapeType p_shape_type, const char *p_shape_type_name, const char *p_role, const char *p_element_type, const Vector<T> &p_vector, bool p_tracker_inventory_complete, ShapeMemoryLinkCounters &r_counters, const char *p_layout = nullptr, uint64_t p_width = 0, uint64_t p_depth = 0) {
	Dictionary row;
	row["shape_rid"] = p_shape_rid;
	row["shape_type"] = int(p_shape_type);
	row["shape_type_name"] = p_shape_type_name;
	row["logical_parent_key"] = p_logical_parent_key;
	row["role"] = p_role;
	row["element_type"] = p_element_type;
	row["element_size_bytes"] = uint64_t(sizeof(T));
	row["size_elements"] = uint64_t(p_vector.size());
	row["capacity_elements"] = uint64_t(p_vector.capacity());
	row["logical_bytes"] = uint64_t(p_vector.size()) * sizeof(T);
	row["capacity_bytes"] = uint64_t(p_vector.capacity()) * sizeof(T);
	if (p_layout != nullptr) {
		row["layout"] = p_layout;
		row["width"] = p_width;
		row["depth"] = p_depth;
	}
	row["cpu_backing"] = _shape_vector_cpu_backing(p_vector, p_tracker_inventory_complete, r_counters);
	r_rows.push_back(row);
}

} // namespace

#define FLUSH_QUERY_CHECK(m_object) \
	ERR_FAIL_COND_MSG(m_object->get_space() && flushing_queries, "Can't change this state while flushing queries. Use call_deferred() or set_deferred() to change monitoring state instead.");

RID GodotPhysicsServer3D::world_boundary_shape_create() {
	GodotShape3D *shape = memnew(GodotWorldBoundaryShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::separation_ray_shape_create() {
	GodotShape3D *shape = memnew(GodotSeparationRayShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::sphere_shape_create() {
	GodotShape3D *shape = memnew(GodotSphereShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::box_shape_create() {
	GodotShape3D *shape = memnew(GodotBoxShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::capsule_shape_create() {
	GodotShape3D *shape = memnew(GodotCapsuleShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::cylinder_shape_create() {
	GodotShape3D *shape = memnew(GodotCylinderShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::convex_polygon_shape_create() {
	GodotShape3D *shape = memnew(GodotConvexPolygonShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::concave_polygon_shape_create() {
	GodotShape3D *shape = memnew(GodotConcavePolygonShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::heightmap_shape_create() {
	GodotShape3D *shape = memnew(GodotHeightMapShape3D);
	RID rid = shape_owner.make_rid(shape);
	shape->set_self(rid);
	return rid;
}
RID GodotPhysicsServer3D::custom_shape_create() {
	ERR_FAIL_V(RID());
}

void GodotPhysicsServer3D::shape_set_data(RID p_shape, const Variant &p_data) {
	GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	shape->set_data(p_data);
}

void GodotPhysicsServer3D::shape_set_custom_solver_bias(RID p_shape, real_t p_bias) {
	GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	shape->set_custom_bias(p_bias);
}

PhysicsServer3D::ShapeType GodotPhysicsServer3D::shape_get_type(RID p_shape) const {
	const GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, SHAPE_CUSTOM);
	return shape->get_type();
}

Variant GodotPhysicsServer3D::shape_get_data(RID p_shape) const {
	const GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, Variant());
	ERR_FAIL_COND_V(!shape->is_configured(), Variant());
	return shape->get_data();
}

Dictionary GodotPhysicsServer3D::get_shape_memory_links() const {
	const uint64_t capture_started_ticks_usec = OS::get_singleton()->get_ticks_usec();
	const uint64_t capture_started_unix_usec = static_cast<uint64_t>(Time::get_singleton()->get_unix_time_from_system() * 1000000.0);
	MemoryTracker::Statistics tracker_before;
	MemoryTracker::get_statistics(tracker_before);

	const uint64_t shape_owner_rid_count = shape_owner.get_rid_count();
	const LocalVector<RID> owned_shape_rids = shape_owner.get_owned_list();
	Array owned_shapes;
	Array rows;
	Array errors;
	uint64_t lookup_error_count = 0;
	uint64_t invalid_shape_count = 0;
	uint64_t concave_shape_count = 0;
	uint64_t heightmap_shape_count = 0;
	uint64_t concave_row_count = 0;
	uint64_t heightmap_row_count = 0;
	ShapeMemoryLinkCounters link_counters;

	for (const RID &shape_rid : owned_shape_rids) {
		const String shape_rid_string = String::num_uint64(shape_rid.get_id());
		const String logical_parent_key = "physics-shape-3d/" + shape_rid_string;
		const GodotShape3D *shape = shape_owner.get_or_null(shape_rid);
		Dictionary shape_info;
		shape_info["shape_rid"] = shape_rid_string;
		shape_info["logical_parent_key"] = logical_parent_key;
		shape_info["lookup_available"] = shape != nullptr;
		if (shape == nullptr) {
			lookup_error_count++;
			shape_info["shape_type_available"] = false;
			shape_info["shape_type"] = -1;
			shape_info["shape_type_name"] = "unknown";
			shape_info["error"] = "shape_owner.get_owned_list() returned a RID which shape_owner.get_or_null() could not resolve.";
			owned_shapes.push_back(shape_info);
			Dictionary error;
			error["code"] = "shape_owner_lookup_failed";
			error["shape_rid"] = shape_rid_string;
			error["logical_parent_key"] = logical_parent_key;
			error["message"] = shape_info["error"];
			errors.push_back(error);
			continue;
		}

		const ShapeType shape_type = shape->get_type();
		shape_info["shape_type_available"] = true;
		shape_info["shape_type"] = int(shape_type);
		shape_info["shape_type_name"] = _shape_type_name(shape_type);
		shape_info["configured"] = shape->is_configured();
		shape_info["is_concave"] = shape->is_concave();
		shape_info["aabb_position"] = shape->get_aabb().position;
		shape_info["aabb_size"] = shape->get_aabb().size;

		if (shape_type == SHAPE_CONCAVE_POLYGON) {
			concave_shape_count++;
			const GodotConcavePolygonShape3D *concave = static_cast<const GodotConcavePolygonShape3D *>(shape);
			const uint64_t face_count = uint64_t(concave->faces.size());
			const uint64_t vertex_count = uint64_t(concave->vertices.size());
			const uint64_t bvh_count = uint64_t(concave->bvh.size());
			const uint64_t expected_vertex_count = face_count * 3;
			const uint64_t expected_bvh_count = face_count == 0 ? 0 : face_count * 2;
			const bool vertex_count_consistent = vertex_count == expected_vertex_count;
			const bool bvh_count_consistent = bvh_count == expected_bvh_count;
			shape_info["face_count"] = face_count;
			shape_info["vertex_count"] = vertex_count;
			shape_info["bvh_storage_node_count"] = bvh_count;
			shape_info["expected_vertex_count"] = expected_vertex_count;
			shape_info["expected_bvh_storage_node_count"] = expected_bvh_count;
			shape_info["vertex_count_consistent"] = vertex_count_consistent;
			shape_info["bvh_count_consistent"] = bvh_count_consistent;
			shape_info["storage_counts_consistent"] = vertex_count_consistent && bvh_count_consistent;

			_append_shape_vector_row(rows, shape_rid_string, logical_parent_key, SHAPE_CONCAVE_POLYGON, "concave_polygon", "faces", "GodotConcavePolygonShape3D::Face", concave->faces, tracker_before.inventory_complete, link_counters);
			_append_shape_vector_row(rows, shape_rid_string, logical_parent_key, SHAPE_CONCAVE_POLYGON, "concave_polygon", "vertices", "Vector3", concave->vertices, tracker_before.inventory_complete, link_counters);
			_append_shape_vector_row(rows, shape_rid_string, logical_parent_key, SHAPE_CONCAVE_POLYGON, "concave_polygon", "bvh", "GodotConcavePolygonShape3D::BVH", concave->bvh, tracker_before.inventory_complete, link_counters);
			concave_row_count += 3;

			if (!vertex_count_consistent || !bvh_count_consistent) {
				invalid_shape_count++;
				Dictionary error;
				error["code"] = "concave_storage_count_mismatch";
				error["shape_rid"] = shape_rid_string;
				error["logical_parent_key"] = logical_parent_key;
				error["face_count"] = face_count;
				error["vertex_count"] = vertex_count;
				error["bvh_storage_node_count"] = bvh_count;
				error["expected_vertex_count"] = expected_vertex_count;
				error["expected_bvh_storage_node_count"] = expected_bvh_count;
				error["message"] = "Concave shape storage counts differ from the current _setup layout; all observed rows are retained.";
				errors.push_back(error);
			}
		} else if (shape_type == SHAPE_HEIGHTMAP) {
			heightmap_shape_count++;
			const GodotHeightMapShape3D *heightmap = static_cast<const GodotHeightMapShape3D *>(shape);
			const uint64_t height_count = uint64_t(heightmap->heights.size());
			const uint64_t width = uint64_t(MAX(heightmap->width, 0));
			const uint64_t depth = uint64_t(MAX(heightmap->depth, 0));
			const uint64_t expected_height_count = width * depth;
			const bool height_count_consistent = height_count == expected_height_count;
			shape_info["height_count"] = height_count;
			shape_info["width"] = width;
			shape_info["depth"] = depth;
			shape_info["expected_height_count"] = expected_height_count;
			shape_info["height_count_consistent"] = height_count_consistent;
			shape_info["storage_counts_consistent"] = height_count_consistent;

			_append_shape_vector_row(rows, shape_rid_string, logical_parent_key, SHAPE_HEIGHTMAP, "heightmap", "heights", "real_t", heightmap->heights, tracker_before.inventory_complete, link_counters, "row_major_z_then_x", width, depth);
			heightmap_row_count++;

			if (!height_count_consistent) {
				invalid_shape_count++;
				Dictionary error;
				error["code"] = "heightmap_storage_count_mismatch";
				error["shape_rid"] = shape_rid_string;
				error["logical_parent_key"] = logical_parent_key;
				error["height_count"] = height_count;
				error["width"] = width;
				error["depth"] = depth;
				error["expected_height_count"] = expected_height_count;
				error["message"] = "Heightmap shape storage count differs from width * depth; the observed row is retained.";
				errors.push_back(error);
			}
		}
		owned_shapes.push_back(shape_info);
	}

	const bool shape_count_consistent = shape_owner_rid_count == uint64_t(owned_shape_rids.size()) && uint64_t(owned_shapes.size()) == uint64_t(owned_shape_rids.size());
	const uint64_t expected_row_count = concave_shape_count * 3 + heightmap_shape_count;
	const bool concave_row_count_consistent = concave_row_count == concave_shape_count * 3;
	const bool heightmap_row_count_consistent = heightmap_row_count == heightmap_shape_count;
	const bool row_count_consistent = uint64_t(rows.size()) == expected_row_count && uint64_t(rows.size()) == concave_row_count + heightmap_row_count;
	const bool metadata_inventory_complete = shape_count_consistent && concave_row_count_consistent && heightmap_row_count_consistent && row_count_consistent && lookup_error_count == 0 && invalid_shape_count == 0;
	MemoryTracker::Statistics tracker_after;
	MemoryTracker::get_statistics(tracker_after);
	const uint64_t capture_finished_unix_usec = static_cast<uint64_t>(Time::get_singleton()->get_unix_time_from_system() * 1000000.0);
	const uint64_t capture_finished_ticks_usec = OS::get_singleton()->get_ticks_usec();

	Dictionary result;
	result["schema_version"] = 1;
	result["format"] = "anthropolis-physics-shape-memory-links-v1";
	result["supported"] = true;
	result["backend"] = "GodotPhysics3D";
	result["process_id"] = uint64_t(OS::get_singleton()->get_process_id());
	result["capture_started_unix_usec"] = capture_started_unix_usec;
	result["capture_finished_unix_usec"] = capture_finished_unix_usec;
	result["capture_started_ticks_usec"] = capture_started_ticks_usec;
	result["capture_finished_ticks_usec"] = capture_finished_ticks_usec;
	result["tracker_start_unix_usec"] = tracker_before.tracker_start_unix_usec;
	result["snapshot_sequence_before"] = tracker_before.snapshot_sequence;
	result["snapshot_sequence_after"] = tracker_after.snapshot_sequence;
	result["native_allocation_tracking_enabled"] = tracker_before.enabled && tracker_after.enabled;
	result["native_allocation_inventory_complete_for_enabled_memory_api_window"] = tracker_before.inventory_complete && tracker_after.inventory_complete;
	result["native_allocation_coverage_complete"] = false;
	result["allocation_lookup_complete_for_backed_vectors"] = tracker_before.enabled && tracker_after.enabled && tracker_before.inventory_complete && tracker_after.inventory_complete && link_counters.allocation_lookup_attempt_count == link_counters.allocation_lookup_success_count;
	result["inventory_capped"] = false;
	result["cross_inventory_join_non_atomic"] = true;
	result["shape_owner_rid_count"] = shape_owner_rid_count;
	result["owned_shape_count"] = uint64_t(owned_shapes.size());
	result["concave_shape_count"] = concave_shape_count;
	result["heightmap_shape_count"] = heightmap_shape_count;
	result["concave_row_count"] = concave_row_count;
	result["heightmap_row_count"] = heightmap_row_count;
	result["expected_row_count"] = expected_row_count;
	result["row_count"] = uint64_t(rows.size());
	result["lookup_error_count"] = lookup_error_count;
	result["invalid_shape_count"] = invalid_shape_count;
	result["error_count"] = lookup_error_count + invalid_shape_count;
	result["allocation_lookup_attempt_count"] = link_counters.allocation_lookup_attempt_count;
	result["allocation_lookup_success_count"] = link_counters.allocation_lookup_success_count;
	result["allocation_lookup_unavailable_count"] = link_counters.allocation_lookup_unavailable_count;
	result["empty_backing_count"] = link_counters.empty_backing_count;
	result["shape_count_consistent"] = shape_count_consistent;
	result["concave_row_count_consistent"] = concave_row_count_consistent;
	result["heightmap_row_count_consistent"] = heightmap_row_count_consistent;
	result["row_count_consistent"] = row_count_consistent;
	result["metadata_inventory_complete"] = metadata_inventory_complete;
	result["owned_shapes"] = owned_shapes;
	result["rows"] = rows;
	result["errors"] = errors;
	result["coverage_note"] = "Uncapped metadata-only enumeration of GodotPhysics3D shape_owner. Exactly three direct Vector rows are emitted per resolved concave-polygon shape and exactly one direct heights Vector row per resolved heightmap shape without calling shape_get_data/get_faces or copying shape storage. Empty and untracked backings remain explicit. Native allocation coverage is never declared complete because pre-enable and excluded allocations remain possible. Result-container and RID-list diagnostic storage is excluded.";
	return result;
}

void GodotPhysicsServer3D::shape_set_margin(RID p_shape, real_t p_margin) {
}

real_t GodotPhysicsServer3D::shape_get_margin(RID p_shape) const {
	return 0.0;
}

real_t GodotPhysicsServer3D::shape_get_custom_solver_bias(RID p_shape) const {
	const GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, 0);
	return shape->get_custom_bias();
}

RID GodotPhysicsServer3D::space_create() {
	GodotSpace3D *space = memnew(GodotSpace3D);
	RID id = space_owner.make_rid(space);
	space->set_self(id);
	RID area_id = area_create();
	GodotArea3D *area = area_owner.get_or_null(area_id);
	ERR_FAIL_NULL_V(area, RID());
	space->set_default_area(area);
	area->set_space(space);
	area->set_priority(-1);
	RID sgb = body_create();
	body_set_space(sgb, id);
	body_set_mode(sgb, BODY_MODE_STATIC);
	space->set_static_global_body(sgb);

	return id;
}

void GodotPhysicsServer3D::space_set_active(RID p_space, bool p_active) {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL(space);
	if (p_active) {
		active_spaces.insert(space);
	} else {
		active_spaces.erase(space);
	}
}

bool GodotPhysicsServer3D::space_is_active(RID p_space) const {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, false);

	return active_spaces.has(space);
}

void GodotPhysicsServer3D::space_set_param(RID p_space, SpaceParameter p_param, real_t p_value) {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL(space);

	space->set_param(p_param, p_value);
}

real_t GodotPhysicsServer3D::space_get_param(RID p_space, SpaceParameter p_param) const {
	const GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, 0);
	return space->get_param(p_param);
}

PhysicsDirectSpaceState3D *GodotPhysicsServer3D::space_get_direct_state(RID p_space) {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, nullptr);
	ERR_FAIL_COND_V_MSG((using_threads && !doing_sync) || space->is_locked(), nullptr, "Space state is inaccessible right now, wait for iteration or physics process notification.");

	return space->get_direct_state();
}

void GodotPhysicsServer3D::space_set_debug_contacts(RID p_space, int p_max_contacts) {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL(space);
	space->set_debug_contacts(p_max_contacts);
}

Vector<Vector3> GodotPhysicsServer3D::space_get_contacts(RID p_space) const {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, Vector<Vector3>());
	return space->get_debug_contacts();
}

int GodotPhysicsServer3D::space_get_contact_count(RID p_space) const {
	GodotSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, 0);
	return space->get_debug_contact_count();
}

RID GodotPhysicsServer3D::area_create() {
	GodotArea3D *area = memnew(GodotArea3D);
	RID rid = area_owner.make_rid(area);
	area->set_self(rid);
	return rid;
}

void GodotPhysicsServer3D::area_set_space(RID p_area, RID p_space) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	GodotSpace3D *space = nullptr;
	if (p_space.is_valid()) {
		space = space_owner.get_or_null(p_space);
		ERR_FAIL_NULL(space);
	}

	if (area->get_space() == space) {
		return; //pointless
	}

	area->clear_constraints();
	area->set_space(space);
}

RID GodotPhysicsServer3D::area_get_space(RID p_area) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, RID());

	GodotSpace3D *space = area->get_space();
	if (!space) {
		return RID();
	}
	return space->get_self();
}

void GodotPhysicsServer3D::area_add_shape(RID p_area, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);

	area->add_shape(shape, p_transform, p_disabled);
}

void GodotPhysicsServer3D::area_set_shape(RID p_area, int p_shape_idx, RID p_shape) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	ERR_FAIL_COND(!shape->is_configured());

	area->set_shape(p_shape_idx, shape);
}

void GodotPhysicsServer3D::area_set_shape_transform(RID p_area, int p_shape_idx, const Transform3D &p_transform) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->set_shape_transform(p_shape_idx, p_transform);
}

int GodotPhysicsServer3D::area_get_shape_count(RID p_area) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, -1);

	return area->get_shape_count();
}

RID GodotPhysicsServer3D::area_get_shape(RID p_area, int p_shape_idx) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, RID());

	GodotShape3D *shape = area->get_shape(p_shape_idx);
	ERR_FAIL_NULL_V(shape, RID());

	return shape->get_self();
}

Transform3D GodotPhysicsServer3D::area_get_shape_transform(RID p_area, int p_shape_idx) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, Transform3D());

	return area->get_shape_transform(p_shape_idx);
}

void GodotPhysicsServer3D::area_remove_shape(RID p_area, int p_shape_idx) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->remove_shape(p_shape_idx);
}

void GodotPhysicsServer3D::area_clear_shapes(RID p_area) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	while (area->get_shape_count()) {
		area->remove_shape(0);
	}
}

void GodotPhysicsServer3D::area_set_shape_disabled(RID p_area, int p_shape_idx, bool p_disabled) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	ERR_FAIL_INDEX(p_shape_idx, area->get_shape_count());
	FLUSH_QUERY_CHECK(area);
	area->set_shape_disabled(p_shape_idx, p_disabled);
}

void GodotPhysicsServer3D::area_attach_object_instance_id(RID p_area, ObjectID p_id) {
	if (space_owner.owns(p_area)) {
		GodotSpace3D *space = space_owner.get_or_null(p_area);
		p_area = space->get_default_area()->get_self();
	}
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_instance_id(p_id);
}

ObjectID GodotPhysicsServer3D::area_get_object_instance_id(RID p_area) const {
	if (space_owner.owns(p_area)) {
		GodotSpace3D *space = space_owner.get_or_null(p_area);
		p_area = space->get_default_area()->get_self();
	}
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, ObjectID());
	return area->get_instance_id();
}

void GodotPhysicsServer3D::area_set_param(RID p_area, AreaParameter p_param, const Variant &p_value) {
	if (space_owner.owns(p_area)) {
		GodotSpace3D *space = space_owner.get_or_null(p_area);
		p_area = space->get_default_area()->get_self();
	}
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_param(p_param, p_value);
}

void GodotPhysicsServer3D::area_set_transform(RID p_area, const Transform3D &p_transform) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_transform(p_transform);
}

Variant GodotPhysicsServer3D::area_get_param(RID p_area, AreaParameter p_param) const {
	if (space_owner.owns(p_area)) {
		GodotSpace3D *space = space_owner.get_or_null(p_area);
		p_area = space->get_default_area()->get_self();
	}
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, Variant());

	return area->get_param(p_param);
}

Transform3D GodotPhysicsServer3D::area_get_transform(RID p_area) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, Transform3D());

	return area->get_transform();
}

void GodotPhysicsServer3D::area_set_collision_layer(RID p_area, uint32_t p_layer) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->set_collision_layer(p_layer);
}

uint32_t GodotPhysicsServer3D::area_get_collision_layer(RID p_area) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, 0);

	return area->get_collision_layer();
}

void GodotPhysicsServer3D::area_set_collision_mask(RID p_area, uint32_t p_mask) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->set_collision_mask(p_mask);
}

uint32_t GodotPhysicsServer3D::area_get_collision_mask(RID p_area) const {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, 0);

	return area->get_collision_mask();
}

void GodotPhysicsServer3D::area_set_monitorable(RID p_area, bool p_monitorable) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	FLUSH_QUERY_CHECK(area);

	area->set_monitorable(p_monitorable);
}

void GodotPhysicsServer3D::area_set_monitor_callback(RID p_area, const Callable &p_callback) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->set_monitor_callback(p_callback.is_valid() ? p_callback : Callable());
}

void GodotPhysicsServer3D::area_set_ray_pickable(RID p_area, bool p_enable) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->set_ray_pickable(p_enable);
}

void GodotPhysicsServer3D::area_set_area_monitor_callback(RID p_area, const Callable &p_callback) {
	GodotArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);

	area->set_area_monitor_callback(p_callback.is_valid() ? p_callback : Callable());
}

/* BODY API */

RID GodotPhysicsServer3D::body_create() {
	GodotBody3D *body = memnew(GodotBody3D);
	RID rid = body_owner.make_rid(body);
	body->set_self(rid);
	return rid;
}

void GodotPhysicsServer3D::body_set_space(RID p_body, RID p_space) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	GodotSpace3D *space = nullptr;
	if (p_space.is_valid()) {
		space = space_owner.get_or_null(p_space);
		ERR_FAIL_NULL(space);
	}

	if (body->get_space() == space) {
		return; //pointless
	}

	body->clear_constraint_map();
	body->set_space(space);
}

RID GodotPhysicsServer3D::body_get_space(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, RID());

	GodotSpace3D *space = body->get_space();
	if (!space) {
		return RID();
	}
	return space->get_self();
}

void GodotPhysicsServer3D::body_set_mode(RID p_body, BodyMode p_mode) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_mode(p_mode);
}

PhysicsServer3D::BodyMode GodotPhysicsServer3D::body_get_mode(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, BODY_MODE_STATIC);

	return body->get_mode();
}

void GodotPhysicsServer3D::body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);

	body->add_shape(shape, p_transform, p_disabled);
}

void GodotPhysicsServer3D::body_set_shape(RID p_body, int p_shape_idx, RID p_shape) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	GodotShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	ERR_FAIL_COND(!shape->is_configured());

	body->set_shape(p_shape_idx, shape);
}
void GodotPhysicsServer3D::body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_shape_transform(p_shape_idx, p_transform);
}

int GodotPhysicsServer3D::body_get_shape_count(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, -1);

	return body->get_shape_count();
}

RID GodotPhysicsServer3D::body_get_shape(RID p_body, int p_shape_idx) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, RID());

	GodotShape3D *shape = body->get_shape(p_shape_idx);
	ERR_FAIL_NULL_V(shape, RID());

	return shape->get_self();
}

void GodotPhysicsServer3D::body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_INDEX(p_shape_idx, body->get_shape_count());
	FLUSH_QUERY_CHECK(body);

	body->set_shape_disabled(p_shape_idx, p_disabled);
}

Transform3D GodotPhysicsServer3D::body_get_shape_transform(RID p_body, int p_shape_idx) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Transform3D());

	return body->get_shape_transform(p_shape_idx);
}

void GodotPhysicsServer3D::body_remove_shape(RID p_body, int p_shape_idx) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->remove_shape(p_shape_idx);
}

void GodotPhysicsServer3D::body_clear_shapes(RID p_body) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	while (body->get_shape_count()) {
		body->remove_shape(0);
	}
}

void GodotPhysicsServer3D::body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_continuous_collision_detection(p_enable);
}

bool GodotPhysicsServer3D::body_is_continuous_collision_detection_enabled(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, false);

	return body->is_continuous_collision_detection_enabled();
}

void GodotPhysicsServer3D::body_set_collision_layer(RID p_body, uint32_t p_layer) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_collision_layer(p_layer);
}

uint32_t GodotPhysicsServer3D::body_get_collision_layer(RID p_body) const {
	const GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);

	return body->get_collision_layer();
}

void GodotPhysicsServer3D::body_set_collision_mask(RID p_body, uint32_t p_mask) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_collision_mask(p_mask);
}

uint32_t GodotPhysicsServer3D::body_get_collision_mask(RID p_body) const {
	const GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);

	return body->get_collision_mask();
}

void GodotPhysicsServer3D::body_set_collision_priority(RID p_body, real_t p_priority) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_collision_priority(p_priority);
}

real_t GodotPhysicsServer3D::body_get_collision_priority(RID p_body) const {
	const GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);

	return body->get_collision_priority();
}

void GodotPhysicsServer3D::body_attach_object_instance_id(RID p_body, ObjectID p_id) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	if (body) {
		body->set_instance_id(p_id);
		return;
	}

	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	if (soft_body) {
		soft_body->set_instance_id(p_id);
		return;
	}

	ERR_FAIL_MSG("Invalid ID.");
}

ObjectID GodotPhysicsServer3D::body_get_object_instance_id(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, ObjectID());

	return body->get_instance_id();
}

void GodotPhysicsServer3D::body_set_user_flags(RID p_body, uint32_t p_flags) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
}

uint32_t GodotPhysicsServer3D::body_get_user_flags(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);

	return 0;
}

void GodotPhysicsServer3D::body_set_param(RID p_body, BodyParameter p_param, const Variant &p_value) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_param(p_param, p_value);
}

Variant GodotPhysicsServer3D::body_get_param(RID p_body, BodyParameter p_param) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);

	return body->get_param(p_param);
}

void GodotPhysicsServer3D::body_reset_mass_properties(RID p_body) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	return body->reset_mass_properties();
}

void GodotPhysicsServer3D::body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_state(p_state, p_variant);
}

Variant GodotPhysicsServer3D::body_get_state(RID p_body, BodyState p_state) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());

	return body->get_state(p_state);
}

void GodotPhysicsServer3D::body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	_update_shapes();

	body->apply_central_impulse(p_impulse);
	body->wakeup();
}

void GodotPhysicsServer3D::body_apply_impulse(RID p_body, const Vector3 &p_impulse, const Vector3 &p_position) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	_update_shapes();

	body->apply_impulse(p_impulse, p_position);
	body->wakeup();
}

void GodotPhysicsServer3D::body_apply_torque_impulse(RID p_body, const Vector3 &p_impulse) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	_update_shapes();

	body->apply_torque_impulse(p_impulse);
	body->wakeup();
}

void GodotPhysicsServer3D::body_apply_central_force(RID p_body, const Vector3 &p_force) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->apply_central_force(p_force);
	body->wakeup();
}

void GodotPhysicsServer3D::body_apply_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->apply_force(p_force, p_position);
	body->wakeup();
}

void GodotPhysicsServer3D::soft_body_apply_point_impulse(RID p_body, int p_point_index, const Vector3 &p_impulse) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->apply_node_impulse(p_point_index, p_impulse);
}

void GodotPhysicsServer3D::soft_body_apply_point_force(RID p_body, int p_point_index, const Vector3 &p_force) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->apply_node_force(p_point_index, p_force);
}

void GodotPhysicsServer3D::soft_body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->apply_central_impulse(p_impulse);
}

void GodotPhysicsServer3D::soft_body_apply_central_force(RID p_body, const Vector3 &p_force) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->apply_central_force(p_force);
}

void GodotPhysicsServer3D::body_apply_torque(RID p_body, const Vector3 &p_torque) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->apply_torque(p_torque);
	body->wakeup();
}

void GodotPhysicsServer3D::body_add_constant_central_force(RID p_body, const Vector3 &p_force) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->add_constant_central_force(p_force);
	body->wakeup();
}

void GodotPhysicsServer3D::body_add_constant_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->add_constant_force(p_force, p_position);
	body->wakeup();
}

void GodotPhysicsServer3D::body_add_constant_torque(RID p_body, const Vector3 &p_torque) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->add_constant_torque(p_torque);
	body->wakeup();
}

void GodotPhysicsServer3D::body_set_constant_force(RID p_body, const Vector3 &p_force) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_constant_force(p_force);
	if (!p_force.is_zero_approx()) {
		body->wakeup();
	}
}

Vector3 GodotPhysicsServer3D::body_get_constant_force(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Vector3());
	return body->get_constant_force();
}

void GodotPhysicsServer3D::body_set_constant_torque(RID p_body, const Vector3 &p_torque) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_constant_torque(p_torque);
	if (!p_torque.is_zero_approx()) {
		body->wakeup();
	}
}

Vector3 GodotPhysicsServer3D::body_get_constant_torque(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Vector3());

	return body->get_constant_torque();
}

void GodotPhysicsServer3D::body_set_axis_velocity(RID p_body, const Vector3 &p_axis_velocity) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	_update_shapes();

	Vector3 v = body->get_linear_velocity();
	Vector3 axis = p_axis_velocity.normalized();
	v -= axis * axis.dot(v);
	v += p_axis_velocity;
	body->set_linear_velocity(v);
	body->wakeup();
}

void GodotPhysicsServer3D::body_set_axis_lock(RID p_body, BodyAxis p_axis, bool p_lock) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_axis_lock(p_axis, p_lock);
	body->wakeup();
}

bool GodotPhysicsServer3D::body_is_axis_locked(RID p_body, BodyAxis p_axis) const {
	const GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, false);
	return body->is_axis_locked(p_axis);
}

void GodotPhysicsServer3D::body_add_collision_exception(RID p_body, RID p_body_b) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->add_exception(p_body_b);
	body->wakeup();
}

void GodotPhysicsServer3D::body_remove_collision_exception(RID p_body, RID p_body_b) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->remove_exception(p_body_b);
	body->wakeup();
}

void GodotPhysicsServer3D::body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	for (int i = 0; i < body->get_exceptions().size(); i++) {
		p_exceptions->push_back(body->get_exceptions()[i]);
	}
}

void GodotPhysicsServer3D::body_set_contacts_reported_depth_threshold(RID p_body, real_t p_threshold) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
}

real_t GodotPhysicsServer3D::body_get_contacts_reported_depth_threshold(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return 0;
}

void GodotPhysicsServer3D::body_set_omit_force_integration(RID p_body, bool p_omit) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);

	body->set_omit_force_integration(p_omit);
}

bool GodotPhysicsServer3D::body_is_omitting_force_integration(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, false);
	return body->get_omit_force_integration();
}

void GodotPhysicsServer3D::body_set_max_contacts_reported(RID p_body, int p_contacts) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_max_contacts_reported(p_contacts);
}

int GodotPhysicsServer3D::body_get_max_contacts_reported(RID p_body) const {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, -1);
	return body->get_max_contacts_reported();
}

void GodotPhysicsServer3D::body_set_state_sync_callback(RID p_body, const Callable &p_callable) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_state_sync_callback(p_callable);
}

void GodotPhysicsServer3D::body_set_force_integration_callback(RID p_body, const Callable &p_callable, const Variant &p_udata) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_force_integration_callback(p_callable, p_udata);
}

void GodotPhysicsServer3D::body_set_ray_pickable(RID p_body, bool p_enable) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_ray_pickable(p_enable);
}

bool GodotPhysicsServer3D::body_test_motion(RID p_body, const MotionParameters &p_parameters, MotionResult *r_result) {
	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, false);
	ERR_FAIL_NULL_V(body->get_space(), false);
	ERR_FAIL_COND_V(body->get_space()->is_locked(), false);

	_update_shapes();

	return body->get_space()->test_body_motion(body, p_parameters, r_result);
}

PhysicsDirectBodyState3D *GodotPhysicsServer3D::body_get_direct_state(RID p_body) {
	ERR_FAIL_COND_V_MSG((using_threads && !doing_sync), nullptr, "Body state is inaccessible right now, wait for iteration or physics process notification.");

	if (!body_owner.owns(p_body)) {
		return nullptr;
	}

	GodotBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, nullptr);

	if (!body->get_space()) {
		return nullptr;
	}

	ERR_FAIL_COND_V_MSG(body->get_space()->is_locked(), nullptr, "Body state is inaccessible right now, wait for iteration or physics process notification.");

	return body->get_direct_state();
}

/* SOFT BODY */

RID GodotPhysicsServer3D::soft_body_create() {
	GodotSoftBody3D *soft_body = memnew(GodotSoftBody3D);
	RID rid = soft_body_owner.make_rid(soft_body);
	soft_body->set_self(rid);
	return rid;
}

void GodotPhysicsServer3D::soft_body_update_rendering_server(RID p_body, RequiredParam<PhysicsServer3DRenderingServerHandler> rp_rendering_server_handler) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);
	EXTRACT_PARAM_OR_FAIL(p_rendering_server_handler, rp_rendering_server_handler);

	soft_body->update_rendering_server(p_rendering_server_handler);
}

void GodotPhysicsServer3D::soft_body_set_space(RID p_body, RID p_space) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	GodotSpace3D *space = nullptr;
	if (p_space.is_valid()) {
		space = space_owner.get_or_null(p_space);
		ERR_FAIL_NULL(space);
	}

	if (soft_body->get_space() == space) {
		return;
	}

	soft_body->set_space(space);
}

RID GodotPhysicsServer3D::soft_body_get_space(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, RID());

	GodotSpace3D *space = soft_body->get_space();
	if (!space) {
		return RID();
	}
	return space->get_self();
}

void GodotPhysicsServer3D::soft_body_set_collision_layer(RID p_body, uint32_t p_layer) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_collision_layer(p_layer);
}

uint32_t GodotPhysicsServer3D::soft_body_get_collision_layer(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0);

	return soft_body->get_collision_layer();
}

void GodotPhysicsServer3D::soft_body_set_collision_mask(RID p_body, uint32_t p_mask) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_collision_mask(p_mask);
}

uint32_t GodotPhysicsServer3D::soft_body_get_collision_mask(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0);

	return soft_body->get_collision_mask();
}

void GodotPhysicsServer3D::soft_body_add_collision_exception(RID p_body, RID p_body_b) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->add_exception(p_body_b);
}

void GodotPhysicsServer3D::soft_body_remove_collision_exception(RID p_body, RID p_body_b) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->remove_exception(p_body_b);
}

void GodotPhysicsServer3D::soft_body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	for (int i = 0; i < soft_body->get_exceptions().size(); i++) {
		p_exceptions->push_back(soft_body->get_exceptions()[i]);
	}
}

void GodotPhysicsServer3D::soft_body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_state(p_state, p_variant);
}

Variant GodotPhysicsServer3D::soft_body_get_state(RID p_body, BodyState p_state) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, Variant());

	return soft_body->get_state(p_state);
}

void GodotPhysicsServer3D::soft_body_set_transform(RID p_body, const Transform3D &p_transform) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_state(BODY_STATE_TRANSFORM, p_transform);
}

void GodotPhysicsServer3D::soft_body_set_ray_pickable(RID p_body, bool p_enable) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_ray_pickable(p_enable);
}

void GodotPhysicsServer3D::soft_body_set_simulation_precision(RID p_body, int p_simulation_precision) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_iteration_count(p_simulation_precision);
}

int GodotPhysicsServer3D::soft_body_get_simulation_precision(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_iteration_count();
}

void GodotPhysicsServer3D::soft_body_set_total_mass(RID p_body, real_t p_total_mass) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_total_mass(p_total_mass);
}

real_t GodotPhysicsServer3D::soft_body_get_total_mass(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_total_mass();
}

void GodotPhysicsServer3D::soft_body_set_linear_stiffness(RID p_body, real_t p_stiffness) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_linear_stiffness(p_stiffness);
}

real_t GodotPhysicsServer3D::soft_body_get_linear_stiffness(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_linear_stiffness();
}

void GodotPhysicsServer3D::soft_body_set_shrinking_factor(RID p_body, real_t p_shrinking_factor) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_shrinking_factor(p_shrinking_factor);
}

real_t GodotPhysicsServer3D::soft_body_get_shrinking_factor(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_shrinking_factor();
}

void GodotPhysicsServer3D::soft_body_set_pressure_coefficient(RID p_body, real_t p_pressure_coefficient) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_pressure_coefficient(p_pressure_coefficient);
}

real_t GodotPhysicsServer3D::soft_body_get_pressure_coefficient(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_pressure_coefficient();
}

void GodotPhysicsServer3D::soft_body_set_damping_coefficient(RID p_body, real_t p_damping_coefficient) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_damping_coefficient(p_damping_coefficient);
}

real_t GodotPhysicsServer3D::soft_body_get_damping_coefficient(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_damping_coefficient();
}

void GodotPhysicsServer3D::soft_body_set_drag_coefficient(RID p_body, real_t p_drag_coefficient) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_drag_coefficient(p_drag_coefficient);
}

real_t GodotPhysicsServer3D::soft_body_get_drag_coefficient(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, 0.f);

	return soft_body->get_drag_coefficient();
}

void GodotPhysicsServer3D::soft_body_set_mesh(RID p_body, RID p_mesh) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_mesh(p_mesh);
}

AABB GodotPhysicsServer3D::soft_body_get_bounds(RID p_body) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, AABB());

	return soft_body->get_bounds();
}

void GodotPhysicsServer3D::soft_body_move_point(RID p_body, int p_point_index, const Vector3 &p_global_position) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->set_vertex_position(p_point_index, p_global_position);
}

Vector3 GodotPhysicsServer3D::soft_body_get_point_global_position(RID p_body, int p_point_index) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, Vector3());

	return soft_body->get_vertex_position(p_point_index);
}

void GodotPhysicsServer3D::soft_body_remove_all_pinned_points(RID p_body) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	soft_body->unpin_all_vertices();
}

void GodotPhysicsServer3D::soft_body_pin_point(RID p_body, int p_point_index, bool p_pin) {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	if (p_pin) {
		soft_body->pin_vertex(p_point_index);
	} else {
		soft_body->unpin_vertex(p_point_index);
	}
}

bool GodotPhysicsServer3D::soft_body_is_point_pinned(RID p_body, int p_point_index) const {
	GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(soft_body, false);

	return soft_body->is_vertex_pinned(p_point_index);
}

/* JOINT API */

RID GodotPhysicsServer3D::joint_create() {
	GodotJoint3D *joint = memnew(GodotJoint3D);
	RID rid = joint_owner.make_rid(joint);
	joint->set_self(rid);
	return rid;
}

void GodotPhysicsServer3D::joint_clear(RID p_joint) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	if (joint->get_type() != JOINT_TYPE_MAX) {
		GodotJoint3D *empty_joint = memnew(GodotJoint3D);
		empty_joint->copy_settings_from(joint);

		joint_owner.replace(p_joint, empty_joint);
		memdelete(joint);
	}
}

void GodotPhysicsServer3D::joint_make_pin(RID p_joint, RID p_body_A, const Vector3 &p_local_A, RID p_body_B, const Vector3 &p_local_B) {
	GodotBody3D *body_A = body_owner.get_or_null(p_body_A);
	ERR_FAIL_NULL(body_A);

	if (!p_body_B.is_valid()) {
		ERR_FAIL_NULL(body_A->get_space());
		p_body_B = body_A->get_space()->get_static_global_body();
	}

	GodotBody3D *body_B = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_B);

	ERR_FAIL_COND(body_A == body_B);

	GodotJoint3D *prev_joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(prev_joint);

	GodotJoint3D *joint = memnew(GodotPinJoint3D(body_A, p_local_A, body_B, p_local_B));

	joint->copy_settings_from(prev_joint);
	joint_owner.replace(p_joint, joint);
	memdelete(prev_joint);
}

void GodotPhysicsServer3D::pin_joint_set_param(RID p_joint, PinJointParam p_param, real_t p_value) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_PIN);
	GodotPinJoint3D *pin_joint = static_cast<GodotPinJoint3D *>(joint);
	pin_joint->set_param(p_param, p_value);
}

real_t GodotPhysicsServer3D::pin_joint_get_param(RID p_joint, PinJointParam p_param) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_PIN, 0);
	GodotPinJoint3D *pin_joint = static_cast<GodotPinJoint3D *>(joint);
	return pin_joint->get_param(p_param);
}

void GodotPhysicsServer3D::pin_joint_set_local_a(RID p_joint, const Vector3 &p_A) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_PIN);
	GodotPinJoint3D *pin_joint = static_cast<GodotPinJoint3D *>(joint);
	pin_joint->set_pos_a(p_A);
}

Vector3 GodotPhysicsServer3D::pin_joint_get_local_a(RID p_joint) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, Vector3());
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_PIN, Vector3());
	GodotPinJoint3D *pin_joint = static_cast<GodotPinJoint3D *>(joint);
	return pin_joint->get_position_a();
}

void GodotPhysicsServer3D::pin_joint_set_local_b(RID p_joint, const Vector3 &p_B) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_PIN);
	GodotPinJoint3D *pin_joint = static_cast<GodotPinJoint3D *>(joint);
	pin_joint->set_pos_b(p_B);
}

Vector3 GodotPhysicsServer3D::pin_joint_get_local_b(RID p_joint) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, Vector3());
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_PIN, Vector3());
	GodotPinJoint3D *pin_joint = static_cast<GodotPinJoint3D *>(joint);
	return pin_joint->get_position_b();
}

void GodotPhysicsServer3D::joint_make_hinge(RID p_joint, RID p_body_A, const Transform3D &p_frame_A, RID p_body_B, const Transform3D &p_frame_B) {
	GodotBody3D *body_A = body_owner.get_or_null(p_body_A);
	ERR_FAIL_NULL(body_A);

	if (!p_body_B.is_valid()) {
		ERR_FAIL_NULL(body_A->get_space());
		p_body_B = body_A->get_space()->get_static_global_body();
	}

	GodotBody3D *body_B = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_B);

	ERR_FAIL_COND(body_A == body_B);

	GodotJoint3D *prev_joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(prev_joint);

	GodotJoint3D *joint = memnew(GodotHingeJoint3D(body_A, body_B, p_frame_A, p_frame_B));

	joint->copy_settings_from(prev_joint);
	joint_owner.replace(p_joint, joint);
	memdelete(prev_joint);
}

void GodotPhysicsServer3D::joint_make_hinge_simple(RID p_joint, RID p_body_A, const Vector3 &p_pivot_A, const Vector3 &p_axis_A, RID p_body_B, const Vector3 &p_pivot_B, const Vector3 &p_axis_B) {
	GodotBody3D *body_A = body_owner.get_or_null(p_body_A);
	ERR_FAIL_NULL(body_A);

	if (!p_body_B.is_valid()) {
		ERR_FAIL_NULL(body_A->get_space());
		p_body_B = body_A->get_space()->get_static_global_body();
	}

	GodotBody3D *body_B = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_B);

	ERR_FAIL_COND(body_A == body_B);

	GodotJoint3D *prev_joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(prev_joint);

	GodotJoint3D *joint = memnew(GodotHingeJoint3D(body_A, body_B, p_pivot_A, p_pivot_B, p_axis_A, p_axis_B));

	joint->copy_settings_from(prev_joint);
	joint_owner.replace(p_joint, joint);
	memdelete(prev_joint);
}

void GodotPhysicsServer3D::hinge_joint_set_param(RID p_joint, HingeJointParam p_param, real_t p_value) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_HINGE);
	GodotHingeJoint3D *hinge_joint = static_cast<GodotHingeJoint3D *>(joint);
	hinge_joint->set_param(p_param, p_value);
}

real_t GodotPhysicsServer3D::hinge_joint_get_param(RID p_joint, HingeJointParam p_param) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_HINGE, 0);
	GodotHingeJoint3D *hinge_joint = static_cast<GodotHingeJoint3D *>(joint);
	return hinge_joint->get_param(p_param);
}

void GodotPhysicsServer3D::hinge_joint_set_flag(RID p_joint, HingeJointFlag p_flag, bool p_enabled) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_HINGE);
	GodotHingeJoint3D *hinge_joint = static_cast<GodotHingeJoint3D *>(joint);
	hinge_joint->set_flag(p_flag, p_enabled);
}

bool GodotPhysicsServer3D::hinge_joint_get_flag(RID p_joint, HingeJointFlag p_flag) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, false);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_HINGE, false);
	GodotHingeJoint3D *hinge_joint = static_cast<GodotHingeJoint3D *>(joint);
	return hinge_joint->get_flag(p_flag);
}

void GodotPhysicsServer3D::joint_set_solver_priority(RID p_joint, int p_priority) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	joint->set_priority(p_priority);
}

int GodotPhysicsServer3D::joint_get_solver_priority(RID p_joint) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	return joint->get_priority();
}

void GodotPhysicsServer3D::joint_disable_collisions_between_bodies(RID p_joint, bool p_disable) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);

	joint->disable_collisions_between_bodies(p_disable);

	if (2 == joint->get_body_count()) {
		GodotBody3D *body_a = *joint->get_body_ptr();
		GodotBody3D *body_b = *(joint->get_body_ptr() + 1);

		if (p_disable) {
			body_add_collision_exception(body_a->get_self(), body_b->get_self());
			body_add_collision_exception(body_b->get_self(), body_a->get_self());
		} else {
			body_remove_collision_exception(body_a->get_self(), body_b->get_self());
			body_remove_collision_exception(body_b->get_self(), body_a->get_self());
		}
	}
}

bool GodotPhysicsServer3D::joint_is_disabled_collisions_between_bodies(RID p_joint) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, true);

	return joint->is_disabled_collisions_between_bodies();
}

GodotPhysicsServer3D::JointType GodotPhysicsServer3D::joint_get_type(RID p_joint) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, JOINT_TYPE_PIN);
	return joint->get_type();
}

void GodotPhysicsServer3D::joint_make_slider(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
	GodotBody3D *body_A = body_owner.get_or_null(p_body_A);
	ERR_FAIL_NULL(body_A);

	if (!p_body_B.is_valid()) {
		ERR_FAIL_NULL(body_A->get_space());
		p_body_B = body_A->get_space()->get_static_global_body();
	}

	GodotBody3D *body_B = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_B);

	ERR_FAIL_COND(body_A == body_B);

	GodotJoint3D *prev_joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(prev_joint);

	GodotJoint3D *joint = memnew(GodotSliderJoint3D(body_A, body_B, p_local_frame_A, p_local_frame_B));

	joint->copy_settings_from(prev_joint);
	joint_owner.replace(p_joint, joint);
	memdelete(prev_joint);
}

void GodotPhysicsServer3D::slider_joint_set_param(RID p_joint, SliderJointParam p_param, real_t p_value) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_SLIDER);
	GodotSliderJoint3D *slider_joint = static_cast<GodotSliderJoint3D *>(joint);
	slider_joint->set_param(p_param, p_value);
}

real_t GodotPhysicsServer3D::slider_joint_get_param(RID p_joint, SliderJointParam p_param) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_CONE_TWIST, 0);
	GodotSliderJoint3D *slider_joint = static_cast<GodotSliderJoint3D *>(joint);
	return slider_joint->get_param(p_param);
}

void GodotPhysicsServer3D::joint_make_cone_twist(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
	GodotBody3D *body_A = body_owner.get_or_null(p_body_A);
	ERR_FAIL_NULL(body_A);

	if (!p_body_B.is_valid()) {
		ERR_FAIL_NULL(body_A->get_space());
		p_body_B = body_A->get_space()->get_static_global_body();
	}

	GodotBody3D *body_B = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_B);

	ERR_FAIL_COND(body_A == body_B);

	GodotJoint3D *prev_joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(prev_joint);

	GodotJoint3D *joint = memnew(GodotConeTwistJoint3D(body_A, body_B, p_local_frame_A, p_local_frame_B));

	joint->copy_settings_from(prev_joint);
	joint_owner.replace(p_joint, joint);
	memdelete(prev_joint);
}

void GodotPhysicsServer3D::cone_twist_joint_set_param(RID p_joint, ConeTwistJointParam p_param, real_t p_value) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_CONE_TWIST);
	GodotConeTwistJoint3D *cone_twist_joint = static_cast<GodotConeTwistJoint3D *>(joint);
	cone_twist_joint->set_param(p_param, p_value);
}

real_t GodotPhysicsServer3D::cone_twist_joint_get_param(RID p_joint, ConeTwistJointParam p_param) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_CONE_TWIST, 0);
	GodotConeTwistJoint3D *cone_twist_joint = static_cast<GodotConeTwistJoint3D *>(joint);
	return cone_twist_joint->get_param(p_param);
}

void GodotPhysicsServer3D::joint_make_generic_6dof(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
	GodotBody3D *body_A = body_owner.get_or_null(p_body_A);
	ERR_FAIL_NULL(body_A);

	if (!p_body_B.is_valid()) {
		ERR_FAIL_NULL(body_A->get_space());
		p_body_B = body_A->get_space()->get_static_global_body();
	}

	GodotBody3D *body_B = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_B);

	ERR_FAIL_COND(body_A == body_B);

	GodotJoint3D *prev_joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(prev_joint);

	GodotJoint3D *joint = memnew(GodotGeneric6DOFJoint3D(body_A, body_B, p_local_frame_A, p_local_frame_B, true));

	joint->copy_settings_from(prev_joint);
	joint_owner.replace(p_joint, joint);
	memdelete(prev_joint);
}

void GodotPhysicsServer3D::generic_6dof_joint_set_param(RID p_joint, Vector3::Axis p_axis, G6DOFJointAxisParam p_param, real_t p_value) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_6DOF);
	GodotGeneric6DOFJoint3D *generic_6dof_joint = static_cast<GodotGeneric6DOFJoint3D *>(joint);
	generic_6dof_joint->set_param(p_axis, p_param, p_value);
}

real_t GodotPhysicsServer3D::generic_6dof_joint_get_param(RID p_joint, Vector3::Axis p_axis, G6DOFJointAxisParam p_param) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_6DOF, 0);
	GodotGeneric6DOFJoint3D *generic_6dof_joint = static_cast<GodotGeneric6DOFJoint3D *>(joint);
	return generic_6dof_joint->get_param(p_axis, p_param);
}

void GodotPhysicsServer3D::generic_6dof_joint_set_flag(RID p_joint, Vector3::Axis p_axis, G6DOFJointAxisFlag p_flag, bool p_enable) {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != JOINT_TYPE_6DOF);
	GodotGeneric6DOFJoint3D *generic_6dof_joint = static_cast<GodotGeneric6DOFJoint3D *>(joint);
	generic_6dof_joint->set_flag(p_axis, p_flag, p_enable);
}

bool GodotPhysicsServer3D::generic_6dof_joint_get_flag(RID p_joint, Vector3::Axis p_axis, G6DOFJointAxisFlag p_flag) const {
	GodotJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, false);
	ERR_FAIL_COND_V(joint->get_type() != JOINT_TYPE_6DOF, false);
	GodotGeneric6DOFJoint3D *generic_6dof_joint = static_cast<GodotGeneric6DOFJoint3D *>(joint);
	return generic_6dof_joint->get_flag(p_axis, p_flag);
}

void GodotPhysicsServer3D::free_rid(RID p_rid) {
	_update_shapes(); //just in case

	if (shape_owner.owns(p_rid)) {
		GodotShape3D *shape = shape_owner.get_or_null(p_rid);

		while (shape->get_owners().size()) {
			GodotShapeOwner3D *so = shape->get_owners().begin()->key;
			so->remove_shape(shape);
		}

		shape_owner.free(p_rid);
		memdelete(shape);
	} else if (body_owner.owns(p_rid)) {
		GodotBody3D *body = body_owner.get_or_null(p_rid);

		body->set_space(nullptr);

		while (body->get_shape_count()) {
			body->remove_shape(0);
		}

		body_owner.free(p_rid);
		memdelete(body);
	} else if (soft_body_owner.owns(p_rid)) {
		GodotSoftBody3D *soft_body = soft_body_owner.get_or_null(p_rid);

		soft_body->set_space(nullptr);

		soft_body_owner.free(p_rid);
		memdelete(soft_body);
	} else if (area_owner.owns(p_rid)) {
		GodotArea3D *area = area_owner.get_or_null(p_rid);

		area->set_space(nullptr);

		while (area->get_shape_count()) {
			area->remove_shape(0);
		}

		area_owner.free(p_rid);
		memdelete(area);
	} else if (space_owner.owns(p_rid)) {
		GodotSpace3D *space = space_owner.get_or_null(p_rid);

		while (space->get_objects().size()) {
			GodotCollisionObject3D *co = static_cast<GodotCollisionObject3D *>(*space->get_objects().begin());
			co->set_space(nullptr);
		}

		active_spaces.erase(space);
		free_rid(space->get_default_area()->get_self());
		free_rid(space->get_static_global_body());

		space_owner.free(p_rid);
		memdelete(space);
	} else if (joint_owner.owns(p_rid)) {
		GodotJoint3D *joint = joint_owner.get_or_null(p_rid);

		joint_owner.free(p_rid);
		memdelete(joint);

	} else {
		ERR_FAIL_MSG("Invalid ID.");
	}
}

void GodotPhysicsServer3D::set_active(bool p_active) {
	active = p_active;
}

void GodotPhysicsServer3D::init() {
	stepper = memnew(GodotStep3D);
}

void GodotPhysicsServer3D::step(real_t p_step) {
	if (!active) {
		return;
	}

	_update_shapes();

	island_count = 0;
	active_objects = 0;
	collision_pairs = 0;
	for (GodotSpace3D *E : active_spaces) {
		stepper->step(E, p_step);
		island_count += E->get_island_count();
		active_objects += E->get_active_objects();
		collision_pairs += E->get_collision_pairs();
	}
}

void GodotPhysicsServer3D::sync() {
	doing_sync = true;
}

void GodotPhysicsServer3D::flush_queries() {
	if (!active) {
		return;
	}

	flushing_queries = true;

	uint64_t time_beg = OS::get_singleton()->get_ticks_usec();

	for (GodotSpace3D *E : active_spaces) {
		GodotSpace3D *space = E;
		space->call_queries();
	}

	flushing_queries = false;

	if (EngineDebugger::is_profiling("servers")) {
		uint64_t total_time[GodotSpace3D::ELAPSED_TIME_MAX];
		static const char *time_name[GodotSpace3D::ELAPSED_TIME_MAX] = {
			"integrate_forces",
			"generate_islands",
			"setup_constraints",
			"solve_constraints",
			"integrate_velocities"
		};

		for (int i = 0; i < GodotSpace3D::ELAPSED_TIME_MAX; i++) {
			total_time[i] = 0;
		}

		for (const GodotSpace3D *E : active_spaces) {
			for (int i = 0; i < GodotSpace3D::ELAPSED_TIME_MAX; i++) {
				total_time[i] += E->get_elapsed_time(GodotSpace3D::ElapsedTime(i));
			}
		}

		Array values;
		values.resize(GodotSpace3D::ELAPSED_TIME_MAX * 2);
		for (int i = 0; i < GodotSpace3D::ELAPSED_TIME_MAX; i++) {
			values[i * 2 + 0] = time_name[i];
			values[i * 2 + 1] = USEC_TO_SEC(total_time[i]);
		}
		values.push_back("flush_queries");
		values.push_back(USEC_TO_SEC(OS::get_singleton()->get_ticks_usec() - time_beg));

		values.push_front("physics_3d");
		EngineDebugger::profiler_add_frame_data("servers", values);
	}
}

void GodotPhysicsServer3D::end_sync() {
	doing_sync = false;
}

void GodotPhysicsServer3D::finish() {
	memdelete(stepper);
}

int GodotPhysicsServer3D::get_process_info(ProcessInfo p_info) {
	switch (p_info) {
		case INFO_ACTIVE_OBJECTS: {
			return active_objects;
		} break;
		case INFO_COLLISION_PAIRS: {
			return collision_pairs;
		} break;
		case INFO_ISLAND_COUNT: {
			return island_count;
		} break;
	}

	return 0;
}

void GodotPhysicsServer3D::_update_shapes() {
	while (pending_shape_update_list.first()) {
		pending_shape_update_list.first()->self()->_shape_changed();
		pending_shape_update_list.remove(pending_shape_update_list.first());
	}
}

void GodotPhysicsServer3D::_shape_col_cbk(const Vector3 &p_point_A, int p_index_A, const Vector3 &p_point_B, int p_index_B, const Vector3 &normal, void *p_userdata) {
	CollCbkData *cbk = static_cast<CollCbkData *>(p_userdata);

	if (cbk->max == 0) {
		return;
	}

	if (cbk->amount == cbk->max) {
		//find least deep
		real_t min_depth = 1e20;
		int min_depth_idx = 0;
		for (int i = 0; i < cbk->amount; i++) {
			real_t d = cbk->ptr[i * 2 + 0].distance_squared_to(cbk->ptr[i * 2 + 1]);
			if (d < min_depth) {
				min_depth = d;
				min_depth_idx = i;
			}
		}

		real_t d = p_point_A.distance_squared_to(p_point_B);
		if (d < min_depth) {
			return;
		}
		cbk->ptr[min_depth_idx * 2 + 0] = p_point_A;
		cbk->ptr[min_depth_idx * 2 + 1] = p_point_B;

	} else {
		cbk->ptr[cbk->amount * 2 + 0] = p_point_A;
		cbk->ptr[cbk->amount * 2 + 1] = p_point_B;
		cbk->amount++;
	}
}

GodotPhysicsServer3D *GodotPhysicsServer3D::godot_singleton = nullptr;
GodotPhysicsServer3D::GodotPhysicsServer3D(bool p_using_threads) {
	godot_singleton = this;
	GodotBroadPhase3D::create_func = GodotBroadPhase3DBVH::_create;

	using_threads = p_using_threads;
}
