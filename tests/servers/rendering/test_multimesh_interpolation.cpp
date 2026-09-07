/**************************************************************************/
/*  test_multimesh_interpolation.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             Godot Engine                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including   */
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

#include "core/templates/rid_owner.h"
#include "servers/rendering/storage/mesh_storage.h"
#include "tests/test_macros.h"
#include "tests/test_tools.h"

#include "servers/rendering/dummy/storage/mesh_storage.h"

TEST_FORCE_LINK(test_multimesh_interpolation)

namespace TestMultiMeshInterpolation {

class MeshStorage : public RendererDummy::MeshStorage {
	mutable MultiMeshInterpolator interpolator;

public:
	static void restore_dummy_singleton(RendererDummy::MeshStorage *p_storage) {
		singleton = p_storage;
	}

	const MultiMeshInterpolator &get_interpolator() const {
		return interpolator;
	}

	virtual void _multimesh_free(RID p_rid) override {
		_interpolation_data.notify_free_multimesh(p_rid);
		RendererDummy::MeshStorage::_multimesh_free(p_rid);
		interpolator = MultiMeshInterpolator();
	}

	virtual int _multimesh_get_instance_count(RID p_multimesh) const override {
		return interpolator._num_instances;
	}

	virtual MultiMeshInterpolator *_multimesh_get_interpolator(RID p_multimesh) const override {
		return &interpolator;
	}
};

struct DummySingletonRestorer {
	RendererDummy::MeshStorage *previous = nullptr;

	~DummySingletonRestorer() {
		MeshStorage::restore_dummy_singleton(previous);
	}
};

static Vector<float> make_buffer(int p_size, float p_first) {
	Vector<float> buffer;
	buffer.resize(p_size);
	for (int i = 0; i < p_size; i++) {
		buffer.write[i] = p_first + i;
	}
	return buffer;
}

TEST_CASE("[Renderer][MultiMesh] Interpolation vectors are allocated lazily") {
	DummySingletonRestorer singleton_restorer{ RendererDummy::MeshStorage::get_singleton() };
	MeshStorage storage;

	RID multimesh = storage.multimesh_allocate();
	REQUIRE(multimesh.is_valid());
	storage.multimesh_initialize(multimesh);
	storage.multimesh_allocate_data(multimesh, 3, RSE::MULTIMESH_TRANSFORM_3D);

	const RendererMeshStorage::MultiMeshInterpolator &fresh = storage.get_interpolator();
	CHECK_FALSE(fresh.interpolated);
	CHECK(fresh._num_instances == 3);
	CHECK(fresh._stride == 12);
	CHECK(fresh._data_curr.is_empty());
	CHECK(fresh._data_prev.is_empty());
	CHECK(fresh._data_interpolated.is_empty());

	Vector<float> backend_buffer = make_buffer(36, 10.0f);
	storage.multimesh_set_buffer(multimesh, backend_buffer);
	storage.multimesh_set_physics_interpolated(multimesh, true);

	const RendererMeshStorage::MultiMeshInterpolator &enabled = storage.get_interpolator();
	CHECK(enabled.interpolated);
	CHECK(enabled._data_curr == backend_buffer);
	CHECK(enabled._data_prev == backend_buffer);
	CHECK(enabled._data_interpolated == backend_buffer);

	storage.multimesh_allocate_data(multimesh, 2, RSE::MULTIMESH_TRANSFORM_2D, true, true);
	const RendererMeshStorage::MultiMeshInterpolator &resized = storage.get_interpolator();
	CHECK(resized._stride == 16);
	CHECK(resized._data_curr.size() == 32);
	CHECK(resized._data_prev.size() == 32);
	CHECK(resized._data_interpolated.size() == 32);

	storage.multimesh_set_physics_interpolated(multimesh, false);
	CHECK(storage.get_interpolator()._data_curr.size() == 32);
	storage.multimesh_allocate_data(multimesh, 0, RSE::MULTIMESH_TRANSFORM_3D);
	CHECK(storage.get_interpolator()._data_curr.is_empty());
	CHECK(storage.get_interpolator()._data_prev.is_empty());
	CHECK(storage.get_interpolator()._data_interpolated.is_empty());
	storage.multimesh_free(multimesh);
	CHECK_FALSE(storage.owns_multimesh(multimesh));

	RID explicit_buffers = storage.multimesh_allocate();
	storage.multimesh_initialize(explicit_buffers);
	storage.multimesh_allocate_data(explicit_buffers, 2, RSE::MULTIMESH_TRANSFORM_3D);
	REQUIRE(storage.get_interpolator()._data_curr.is_empty());
	Vector<float> current = make_buffer(24, 100.0f);
	Vector<float> previous = make_buffer(24, 200.0f);
	storage.multimesh_set_buffer_interpolated(explicit_buffers, current, previous);
	CHECK_FALSE(storage.get_interpolator().interpolated);
	CHECK(storage.get_interpolator()._data_curr == current);
	CHECK(storage.get_interpolator()._data_prev == previous);
	storage.multimesh_free(explicit_buffers);

	RID empty_history = storage.multimesh_allocate();
	storage.multimesh_initialize(empty_history);
	storage.multimesh_allocate_data(empty_history, 0, RSE::MULTIMESH_TRANSFORM_3D);
	Vector<float> empty;
	storage.multimesh_set_buffer_interpolated(empty_history, empty, empty);
	CHECK(storage.get_interpolator().on_interpolate_update_list);
	CHECK(storage.get_interpolator().on_transform_update_list);
	storage.multimesh_allocate_data(empty_history, 2, RSE::MULTIMESH_TRANSFORM_2D);
	CHECK(storage.get_interpolator()._data_curr.size() == 16);
	CHECK(storage.get_interpolator()._data_prev.size() == 16);
	CHECK(storage.get_interpolator()._data_interpolated.size() == 16);
	storage.multimesh_free(empty_history);

	RID reset_before_cache = storage.multimesh_allocate();
	storage.multimesh_initialize(reset_before_cache);
	storage.multimesh_allocate_data(reset_before_cache, 2, RSE::MULTIMESH_TRANSFORM_3D);
	REQUIRE(storage.get_interpolator()._data_curr.is_empty());
	storage.multimesh_instance_reset_physics_interpolation(reset_before_cache, 1);
	CHECK(storage.get_interpolator()._data_curr.is_empty());

	ErrorDetector error_detector;
	ERR_PRINT_OFF
	storage.multimesh_instance_reset_physics_interpolation(reset_before_cache, 2);
	ERR_PRINT_ON
	CHECK(error_detector.has_error);
	storage.multimesh_free(reset_before_cache);
}

} // namespace TestMultiMeshInterpolation
