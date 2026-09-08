#include "rendering_device.h"

Dictionary RenderingDevice::get_memory_resource_inventory() {
	ERR_FAIL_COND_V_MSG(render_thread_id != Thread::get_caller_id(), Dictionary(), "Memory inventory must run on the rendering thread.");
	// Explicit metadata export. Never wait for GPU idle, map contents, or collect on a frame tick.
	Dictionary result;
	result["resource_inventory_schema_version"] = 1;
	result["snapshot_non_atomic"] = true;
	result["frames_drawn"] = frames_drawn;
	result["driver_statistics_before"] = driver->get_memory_diagnostics();
	result["identity_scope"] = "RID identities belong to this RenderingDevice/process; D3D12 allocation/resource identities include lifetime generations. Driver-record and heap pointers remain capture-local only.";
	Array resources;
	auto record_buffer = [&](RDD::BufferID p_buffer, const String &p_family, const String &p_key, const String &p_lifecycle, uint64_t p_size, RID p_rid = RID()) {
		Dictionary row;
		row["key"] = p_key;
		row["family"] = p_family;
		row["lifecycle"] = p_lifecycle;
		row["rd_rid"] = String::num_uint64(p_rid.get_id());
		row["logical_size_bytes"] = p_size;
		row["driver"] = driver->buffer_get_memory_identity(p_buffer);
		resources.push_back(row);
	};
	// Match normal pool -> worker lock order, before acquiring the RD resource mutex.
	{
		MutexLock pool_lock(transfer_worker_pool_mutex);
		for (uint32_t i = 0; i < transfer_worker_pool_size; i++) {
			TransferWorker *worker = transfer_worker_pool[i];
			MutexLock worker_lock(worker->thread_mutex);
			if (worker->staging_buffer) {
				record_buffer(worker->staging_buffer, "transfer_worker_staging", "transfer/" + itos(i), "worker_pool_capacity", worker->staging_buffer_size_allocated);
				Dictionary row = resources[resources.size() - 1];
				row["used_bytes"] = worker->staging_buffer_size_in_use;
				row["source"] = "servers/rendering/rendering_device.cpp: transfer worker staging allocation";
			}
		}
	}
	{
		_THREAD_SAFE_METHOD_
		const StagingBuffers *rings[] = { &upload_staging_buffers, &download_staging_buffers };
		const char *ring_names[] = { "upload_ring", "download_ring" };
		for (int ring_index = 0; ring_index < 2; ring_index++) {
			uint32_t block_index = 0;
			for (const StagingBufferBlock &block : rings[ring_index]->blocks) {
				record_buffer(block.driver_id, ring_names[ring_index], String(ring_names[ring_index]) + "/" + itos(block_index++), "staging_ring_capacity", rings[ring_index]->block_size);
				Dictionary row = resources[resources.size() - 1];
				row["last_fill_bytes"] = block.fill_amount;
			}
		}
		auto record_owner = [&](auto &p_owner, const char *p_family) {
			for (RID rid : p_owner.get_owned_list()) {
				const Buffer *buffer = p_owner.get_or_null(rid);
				record_buffer(buffer->driver_id, p_family, "rd/" + String::num_uint64(rid.get_id()), "active", buffer->size, rid);
				Dictionary row = resources[resources.size() - 1];
				row["usage_flags"] = uint64_t(buffer->usage);
				if (const String *name = resource_names.getptr(rid)) {
					row["name"] = *name;
				}
			}
		};
		record_owner(uniform_buffer_owner, "uniform_buffer");
		record_owner(storage_buffer_owner, "storage_buffer");
		record_owner(texture_buffer_owner, "texture_buffer");
		record_owner(vertex_buffer_owner, "vertex_buffer");
		record_owner(index_buffer_owner, "index_buffer");
		auto record_texture = [&](const Texture &p_texture, RID p_rid, const String &p_key, const String &p_lifecycle) {
			Dictionary row;
			row["key"] = p_key;
			row["family"] = p_texture.owner.is_null() ? "texture" : "texture_view";
			row["lifecycle"] = p_lifecycle;
			row["rd_rid"] = String::num_uint64(p_rid.get_id());
			row["owner_rd_rid"] = String::num_uint64(p_texture.owner.get_id());
			row["format_enum"] = int(p_texture.format);
			row["type_enum"] = int(p_texture.type);
			row["width"] = p_texture.width;
			row["height"] = p_texture.height;
			row["depth"] = p_texture.depth;
			row["layers"] = p_texture.layers;
			row["mipmaps"] = p_texture.mipmaps;
			row["samples_enum"] = int(p_texture.samples);
			row["usage_flags"] = p_texture.usage_flags;
			row["driver"] = driver->texture_get_memory_identity(p_texture.driver_id);
			if (const String *name = resource_names.getptr(p_rid)) {
				row["name"] = *name;
			}
			resources.push_back(row);
			if (p_texture.shared_fallback != nullptr) {
				if (p_texture.shared_fallback->texture) {
					Dictionary fallback;
					fallback["key"] = p_key + "/fallback_texture";
					fallback["family"] = "shared_fallback_texture";
					fallback["lifecycle"] = p_lifecycle;
					fallback["rd_rid"] = String::num_uint64(p_rid.get_id());
					fallback["logical_parent_key"] = p_key;
					fallback["driver"] = driver->texture_get_memory_identity(p_texture.shared_fallback->texture);
					resources.push_back(fallback);
				}
				if (p_texture.shared_fallback->buffer) {
					record_buffer(p_texture.shared_fallback->buffer, "shared_fallback_buffer", p_key + "/fallback_buffer", p_lifecycle, 0, p_rid);
					Dictionary fallback = resources[resources.size() - 1];
					fallback["logical_parent_key"] = p_key;
					fallback["logical_size_known"] = false;
				}
			}
		};
		for (RID rid : texture_owner.get_owned_list()) {
			record_texture(*texture_owner.get_or_null(rid), rid, "rd/" + String::num_uint64(rid.get_id()), "active");
		}
		for (uint32_t frame_index = 0; frame_index < frames.size(); frame_index++) {
			const Frame &pending = frames[frame_index];
			String prefix = "pending/" + itos(frame_index) + "/";
			uint32_t index = 0;
			for (const Buffer &buffer : pending.buffers_to_dispose_of) {
				record_buffer(buffer.driver_id, "buffer", prefix + "buffer/" + itos(index++), "pending_frame_retirement", buffer.size);
			}
			index = 0;
			for (const Texture &texture : pending.textures_to_dispose_of) {
				record_texture(texture, RID(), prefix + "texture/" + itos(index++), "pending_frame_retirement");
			}
		}
		result["named_resource_count"] = resource_names.size();
		result["names_enabled_from_start"] = memory_inventory_names_enabled;
		result["rd_buffer_accounted_bytes_including_pending"] = buffer_memory;
		result["rd_texture_allocation_bytes_including_pending"] = texture_memory;
	}
	result["resources"] = resources;
	result["resource_row_count"] = resources.size();
	result["rows_capped"] = false;
	result["driver_statistics_after"] = driver->get_memory_diagnostics();
	result["coverage_note"] = "Full rows for RD buffer/texture owners, staging worker/ring buffers and pending buffer/texture destruction. Multiple rows may share an allocation: deduplicate by allocation_id. Transfer-worker rows and RD-owner rows are different epochs; even generation-safe sums are not an instantaneous allocator total. Not a full driver allocation census: acceleration structures, SBT, descriptors, pipelines, swapchain and native CPU storage require further joins. Heap/driver pointers are capture-local; no residency inference.";
	result["diagnostic_overhead_note"] = "D3D12 resource generation adds 8 bytes per driver resource record plus one process counter. Optional resource names retain a RID-to-string map. Export dictionaries/arrays are transient and not included in GPU storage. Native allocator tracing must account for their actual CPU backing.";
	return result;
}
