/**************************************************************************/
/*  jolt_concave_polygon_shape_3d.cpp                                     */
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

#include "jolt_concave_polygon_shape_3d.h"

#include "../jolt_project_settings.h"
#include "../misc/jolt_type_conversions.h"

#include "core/config/project_settings.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"

#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <atomic>

// The worker reads and writes only this, never the shape. The shape owns it until it takes the mesh or abandons
// the build; an abandoned one belongs to the list below until its task has been waited for.
struct JoltConcavePolygonShape3D::PendingBuild {
	// QUEUED moves to RUNNING when the worker starts it, or to CANCELLED when the shape needs it first or drops it.
	enum State {
		QUEUED,
		RUNNING,
		DONE,
		CANCELLED,
	};

	PackedVector3Array faces;
	float active_edge_threshold_cos = 0.0f;
	bool per_triangle_user_data = false;
	std::atomic<int> state = QUEUED;
	SafeFlag abandoned;
	JPH::ShapeRefC mesh;
	String error;
};

namespace {

struct AbandonedBuild {
	WorkerThreadPool::TaskID task;
	void *pending_build; // A JoltConcavePolygonShape3D::PendingBuild, which is private to the shape.
};

Mutex abandoned_builds_mutex;
LocalVector<AbandonedBuild> abandoned_builds;

} // namespace

JoltConcavePolygonShape3D::~JoltConcavePolygonShape3D() {
	_abandon_pending_build();
	_reclaim_abandoned_builds(false);
}

JPH::ShapeRefC JoltConcavePolygonShape3D::_build() const {
	JPH::ShapeRefC mesh;
	String error;

	if (!_take_pending_build(mesh, error)) {
		const int vertex_count = (int)faces.size();
		const int excess_vertex_count = vertex_count % 3;

		if (unlikely(vertex_count == 0)) {
			return nullptr;
		}

		ERR_FAIL_COND_V_MSG(vertex_count < 3, nullptr, vformat("Failed to build Jolt Physics concave polygon shape with %s. It must have a vertex count of at least 3. This shape belongs to %s.", to_string(), _owners_to_string()));
		ERR_FAIL_COND_V_MSG(excess_vertex_count != 0, nullptr, vformat("Failed to build Jolt Physics concave polygon shape with %s. It must have a vertex count that is divisible by 3. This shape belongs to %s.", to_string(), _owners_to_string()));

		mesh = _build_mesh(faces, JoltProjectSettings::active_edge_threshold_cos, JoltProjectSettings::enable_ray_cast_face_index, error);
	}

	ERR_FAIL_NULL_V_MSG(mesh, nullptr, vformat("Failed to build Jolt Physics concave polygon shape with %s. It returned the following error: '%s'. This shape belongs to %s.", to_string(), error, _owners_to_string()));

	return JoltShape3D::with_double_sided(mesh, back_face_collision);
}

JPH::ShapeRefC JoltConcavePolygonShape3D::_build_mesh(const PackedVector3Array &p_faces, float p_active_edge_threshold_cos, bool p_per_triangle_user_data, String &r_error) {
	const int vertex_count = (int)p_faces.size();
	const int face_count = vertex_count / 3;

	JPH::TriangleList jolt_faces;
	jolt_faces.reserve((size_t)face_count);

	const Vector3 *faces_begin = &p_faces[0];
	const Vector3 *faces_end = faces_begin + vertex_count;
	JPH::uint32 triangle_index = 0;

	for (const Vector3 *vertex = faces_begin; vertex != faces_end; vertex += 3) {
		const Vector3 *v0 = vertex + 0;
		const Vector3 *v1 = vertex + 1;
		const Vector3 *v2 = vertex + 2;

		// Jolt uses a different winding order, so we swizzle the vertices to account for that.
		jolt_faces.emplace_back(
				JPH::Float3((float)v2->x, (float)v2->y, (float)v2->z),
				JPH::Float3((float)v1->x, (float)v1->y, (float)v1->z),
				JPH::Float3((float)v0->x, (float)v0->y, (float)v0->z),
				0,
				triangle_index++);
	}

	JPH::MeshShapeSettings shape_settings(jolt_faces);
	shape_settings.mActiveEdgeCosThresholdAngle = p_active_edge_threshold_cos;
	shape_settings.mPerTriangleUserData = p_per_triangle_user_data;

	const JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();

	if (shape_result.HasError()) {
		r_error = to_godot(shape_result.GetError());
		return nullptr;
	}

	return shape_result.Get();
}

void JoltConcavePolygonShape3D::_run_pending_build(void *p_pending_build) {
	PendingBuild *pending = static_cast<PendingBuild *>(p_pending_build);

	int expected = PendingBuild::QUEUED;
	if (!pending->state.compare_exchange_strong(expected, PendingBuild::RUNNING)) {
		return;
	}

	pending->mesh = _build_mesh(pending->faces, pending->active_edge_threshold_cos, pending->per_triangle_user_data, pending->error);
	pending->faces = PackedVector3Array();

	// Abandoned while it ran: drop the mesh now, not when the task is reclaimed.
	if (pending->abandoned.is_set()) {
		pending->mesh = nullptr;
	}

	pending->state.store(PendingBuild::DONE);
}

void JoltConcavePolygonShape3D::_reclaim_abandoned_builds(bool p_wait) {
	MutexLock lock(abandoned_builds_mutex);

	uint32_t kept = 0;

	for (uint32_t i = 0; i < abandoned_builds.size(); ++i) {
		const AbandonedBuild abandoned = abandoned_builds[i];

		if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(abandoned.task)) {
			abandoned_builds[kept++] = abandoned;
			continue;
		}

		WorkerThreadPool::get_singleton()->wait_for_task_completion(abandoned.task);
		memdelete(static_cast<PendingBuild *>(abandoned.pending_build));
	}

	abandoned_builds.resize(kept);
}

void JoltConcavePolygonShape3D::_start_pending_build() {
	const int vertex_count = (int)faces.size();

	// An empty or malformed face list starts nothing: `_build` reports it to the owners as before.
	if (vertex_count < 3 || vertex_count % 3 != 0) {
		return;
	}

	jolt_ref_mutex.lock();
	const bool built = jolt_ref != nullptr;
	jolt_ref_mutex.unlock();

	// An owner already in a space built the shape when `destroy` told it of the change.
	if (built) {
		return;
	}

	PendingBuild *pending = memnew(PendingBuild);
	pending->faces = faces;
	pending->active_edge_threshold_cos = JoltProjectSettings::active_edge_threshold_cos;
	pending->per_triangle_user_data = JoltProjectSettings::enable_ray_cast_face_index;

	pending_build = pending;
	pending_build_task = WorkerThreadPool::get_singleton()->add_native_task(&_run_pending_build, pending, false, "Jolt concave polygon shape build");
}

bool JoltConcavePolygonShape3D::_take_pending_build(JPH::ShapeRefC &r_mesh, String &r_error) const {
	PendingBuild *pending = pending_build;

	if (pending == nullptr) {
		return false;
	}

	// Not started yet: build it here rather than wait behind other low-priority work in the pool.
	int expected = PendingBuild::QUEUED;
	if (pending->state.compare_exchange_strong(expected, PendingBuild::CANCELLED)) {
		_abandon_pending_build();
		return false;
	}

	// Running or done, so the wait is at most the rest of this one build. A pool thread may not wait for an older
	// task (ERR_BUSY); it builds the shape itself.
	if (WorkerThreadPool::get_singleton()->wait_for_task_completion(pending_build_task) != OK) {
		_abandon_pending_build();
		return false;
	}

	r_mesh = pending->mesh;
	r_error = pending->error;

	memdelete(pending);
	pending_build = nullptr;
	pending_build_task = WorkerThreadPool::INVALID_TASK_ID;

	return true;
}

void JoltConcavePolygonShape3D::_abandon_pending_build() const {
	PendingBuild *pending = pending_build;

	if (pending == nullptr) {
		return;
	}

	// One not yet started does nothing, one running drops its mesh when it finishes, and a finished one drops it
	// here. Its task is waited for later, so neither a change nor a free waits.
	pending->abandoned.set();

	int expected = PendingBuild::QUEUED;
	if (!pending->state.compare_exchange_strong(expected, PendingBuild::CANCELLED) && expected == PendingBuild::DONE) {
		pending->mesh = nullptr;
	}

	{
		MutexLock lock(abandoned_builds_mutex);
		abandoned_builds.push_back({ pending_build_task, pending });
	}

	pending_build = nullptr;
	pending_build_task = WorkerThreadPool::INVALID_TASK_ID;
}

AABB JoltConcavePolygonShape3D::_calculate_aabb() const {
	AABB result;

	for (int i = 0; i < faces.size(); ++i) {
		const Vector3 &vertex = faces[i];

		if (i == 0) {
			result.position = vertex;
		} else {
			result.expand_to(vertex);
		}
	}

	return result;
}

Variant JoltConcavePolygonShape3D::get_data() const {
	Dictionary data;
	data["faces"] = faces;
	data["backface_collision"] = back_face_collision;
	return data;
}

void JoltConcavePolygonShape3D::set_data(const Variant &p_data) {
	ERR_FAIL_COND(p_data.get_type() != Variant::DICTIONARY);

	const Dictionary data = p_data;

	const Variant maybe_faces = data.get("faces", Variant());
	ERR_FAIL_COND(maybe_faces.get_type() != Variant::PACKED_VECTOR3_ARRAY);

	const Variant maybe_back_face_collision = data.get("backface_collision", Variant());
	ERR_FAIL_COND(maybe_back_face_collision.get_type() != Variant::BOOL);

	_abandon_pending_build();
	_reclaim_abandoned_builds(false);

	faces = maybe_faces;
	back_face_collision = maybe_back_face_collision;

	aabb = _calculate_aabb();

	destroy();

	// Anthropolis: built from the same faces with the same settings, so the shape is the one the first body to
	// enter a space would have built.
	if (GLOBAL_GET_CACHED(bool, "physics/jolt_physics_3d/anthropolis_build_concave_shapes_async")) {
		_start_pending_build();
	}
}

String JoltConcavePolygonShape3D::to_string() const {
	return vformat("{vertex_count=%d}", faces.size());
}
