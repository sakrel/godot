/*************************************************************************/
/*  animation_tree.cpp                                                   */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "animation_tree.h"

#include "animation_blend_tree.h"
#include "core/config/engine.h"
#include "scene/resources/animation.h"
#include "scene/scene_string_names.h"
#include "servers/audio/audio_stream.h"

void AnimationNode::get_parameter_list(List<PropertyInfo> *r_list) const {
	Array parameters;

	if (GDVIRTUAL_CALL(_get_parameter_list, parameters)) {
		for (int i = 0; i < parameters.size(); i++) {
			Dictionary d = parameters[i];
			ERR_CONTINUE(d.is_empty());
			r_list->push_back(PropertyInfo::from_dict(d));
		}
	}
}

Variant AnimationNode::get_parameter_default_value(const StringName &p_parameter) const {
	Variant ret;
	if (GDVIRTUAL_CALL(_get_parameter_default_value, p_parameter, ret)) {
		return ret;
	}
	return Variant();
}

void AnimationNode::set_parameter(const StringName &p_name, const Variant &p_value) {
	ERR_FAIL_COND(!state);
	ERR_FAIL_COND(!state->tree->property_parent_map.has(base_path));
	ERR_FAIL_COND(!state->tree->property_parent_map[base_path].has(p_name));
	StringName path = state->tree->property_parent_map[base_path][p_name];

	state->tree->property_map[path] = p_value;
}

Variant AnimationNode::get_parameter(const StringName &p_name) const {
	ERR_FAIL_COND_V(!state, Variant());
	ERR_FAIL_COND_V(!state->tree->property_parent_map.has(base_path), Variant());
	ERR_FAIL_COND_V(!state->tree->property_parent_map[base_path].has(p_name), Variant());

	StringName path = state->tree->property_parent_map[base_path][p_name];
	return state->tree->property_map[path];
}

void AnimationNode::get_child_nodes(List<ChildNode> *r_child_nodes) {
	Dictionary cn;
	if (GDVIRTUAL_CALL(_get_child_nodes, cn)) {
		List<Variant> keys;
		cn.get_key_list(&keys);
		for (const Variant &E : keys) {
			ChildNode child;
			child.name = E;
			child.node = cn[E];
			r_child_nodes->push_back(child);
		}
	}
}

void AnimationNode::blend_animation(const StringName &p_animation, double p_time, double p_delta, bool p_seeked, bool p_seek_root, real_t p_blend, int p_pingponged) {
	ERR_FAIL_COND(!state);
	ERR_FAIL_COND(!state->player->has_animation(p_animation));

	Ref<Animation> animation = state->player->get_animation(p_animation);

	if (animation.is_null()) {
		AnimationNodeBlendTree *btree = Object::cast_to<AnimationNodeBlendTree>(parent);
		if (btree) {
			String name = btree->get_node_name(Ref<AnimationNodeAnimation>(this));
			make_invalid(vformat(RTR("In node '%s', invalid animation: '%s'."), name, p_animation));
		} else {
			make_invalid(vformat(RTR("Invalid animation: '%s'."), p_animation));
		}
		return;
	}

	ERR_FAIL_COND(!animation.is_valid());

	AnimationState anim_state;
	anim_state.blend = p_blend;
	anim_state.track_blends = &blends;
	anim_state.delta = p_delta;
	anim_state.time = p_time;
	anim_state.animation = animation;
	anim_state.seeked = p_seeked;
	anim_state.pingponged = p_pingponged;
	anim_state.seek_root = p_seek_root;

	state->animation_states.push_back(anim_state);
}

double AnimationNode::_pre_process(const StringName &p_base_path, AnimationNode *p_parent, State *p_state, double p_time, bool p_seek, bool p_seek_root, const Vector<StringName> &p_connections) {
	base_path = p_base_path;
	parent = p_parent;
	connections = p_connections;
	state = p_state;

	double t = process(p_time, p_seek, p_seek_root);

	state = nullptr;
	parent = nullptr;
	base_path = StringName();
	connections.clear();

	return t;
}

AnimationTree *AnimationNode::get_animation_tree() const {
	ERR_FAIL_COND_V(!state, nullptr);
	return state->tree;
}

void AnimationNode::make_invalid(const String &p_reason) {
	ERR_FAIL_COND(!state);
	state->valid = false;
	if (!state->invalid_reasons.is_empty()) {
		state->invalid_reasons += "\n";
	}
	state->invalid_reasons += String::utf8("•  ") + p_reason;
}

double AnimationNode::blend_input(int p_input, double p_time, bool p_seek, bool p_seek_root, real_t p_blend, FilterAction p_filter, bool p_optimize) {
	ERR_FAIL_INDEX_V(p_input, inputs.size(), 0);
	ERR_FAIL_COND_V(!state, 0);

	AnimationNodeBlendTree *blend_tree = Object::cast_to<AnimationNodeBlendTree>(parent);
	ERR_FAIL_COND_V(!blend_tree, 0);

	StringName node_name = connections[p_input];

	if (!blend_tree->has_node(node_name)) {
		String name = blend_tree->get_node_name(Ref<AnimationNode>(this));
		make_invalid(vformat(RTR("Nothing connected to input '%s' of node '%s'."), get_input_name(p_input), name));
		return 0;
	}

	Ref<AnimationNode> node = blend_tree->get_node(node_name);

	//inputs.write[p_input].last_pass = state->last_pass;
	real_t activity = 0.0;
	double ret = _blend_node(node_name, blend_tree->get_node_connection_array(node_name), nullptr, node, p_time, p_seek, p_seek_root, p_blend, p_filter, p_optimize, &activity);

	Vector<AnimationTree::Activity> *activity_ptr = state->tree->input_activity_map.getptr(base_path);

	if (activity_ptr && p_input < activity_ptr->size()) {
		activity_ptr->write[p_input].last_pass = state->last_pass;
		activity_ptr->write[p_input].activity = activity;
	}
	return ret;
}

double AnimationNode::blend_node(const StringName &p_sub_path, Ref<AnimationNode> p_node, double p_time, bool p_seek, bool p_seek_root, real_t p_blend, FilterAction p_filter, bool p_optimize) {
	return _blend_node(p_sub_path, Vector<StringName>(), this, p_node, p_time, p_seek, p_seek_root, p_blend, p_filter, p_optimize);
}

double AnimationNode::_blend_node(const StringName &p_subpath, const Vector<StringName> &p_connections, AnimationNode *p_new_parent, Ref<AnimationNode> p_node, double p_time, bool p_seek, bool p_seek_root, real_t p_blend, FilterAction p_filter, bool p_optimize, real_t *r_max) {
	ERR_FAIL_COND_V(!p_node.is_valid(), 0);
	ERR_FAIL_COND_V(!state, 0);

	int blend_count = blends.size();

	if (p_node->blends.size() != blend_count) {
		p_node->blends.resize(blend_count);
	}

	real_t *blendw = p_node->blends.ptrw();
	const real_t *blendr = blends.ptr();

	bool any_valid = false;

	if (has_filter() && is_filter_enabled() && p_filter != FILTER_IGNORE) {
		for (int i = 0; i < blend_count; i++) {
			blendw[i] = 0.0; //all to zero by default
		}

		for (const KeyValue<NodePath, bool> &E : filter) {
			if (!state->track_map.has(E.key)) {
				continue;
			}
			int idx = state->track_map[E.key];
			blendw[idx] = 1.0; //filtered goes to one
		}

		switch (p_filter) {
			case FILTER_IGNORE:
				break; //will not happen anyway
			case FILTER_PASS: {
				//values filtered pass, the rest don't
				for (int i = 0; i < blend_count; i++) {
					if (blendw[i] == 0) { //not filtered, does not pass
						continue;
					}

					blendw[i] = blendr[i] * p_blend;
					if (blendw[i] > CMP_EPSILON) {
						any_valid = true;
					}
				}

			} break;
			case FILTER_STOP: {
				//values filtered don't pass, the rest are blended

				for (int i = 0; i < blend_count; i++) {
					if (blendw[i] > 0) { //filtered, does not pass
						continue;
					}

					blendw[i] = blendr[i] * p_blend;
					if (blendw[i] > CMP_EPSILON) {
						any_valid = true;
					}
				}

			} break;
			case FILTER_BLEND: {
				//filtered values are blended, the rest are passed without blending

				for (int i = 0; i < blend_count; i++) {
					if (blendw[i] == 1.0) {
						blendw[i] = blendr[i] * p_blend; //filtered, blend
					} else {
						blendw[i] = blendr[i]; //not filtered, do not blend
					}

					if (blendw[i] > CMP_EPSILON) {
						any_valid = true;
					}
				}

			} break;
		}
	} else {
		for (int i = 0; i < blend_count; i++) {
			//regular blend
			blendw[i] = blendr[i] * p_blend;
			if (blendw[i] > CMP_EPSILON) {
				any_valid = true;
			}
		}
	}

	if (r_max) {
		*r_max = 0;
		for (int i = 0; i < blend_count; i++) {
			*r_max = MAX(*r_max, blendw[i]);
		}
	}

	String new_path;
	AnimationNode *new_parent;

	// This is the slowest part of processing, but as strings process in powers of 2, and the paths always exist, it will not result in that many allocations.
	if (p_new_parent) {
		new_parent = p_new_parent;
		new_path = String(base_path) + String(p_subpath) + "/";
	} else {
		ERR_FAIL_COND_V(!parent, 0);
		new_parent = parent;
		new_path = String(parent->base_path) + String(p_subpath) + "/";
	}

	// If tracks for blending don't exist for one of the animations, Rest or RESET animation is blended as init animation instead.
	// Then, blend weight is 0 means that the init animation blend weight is 1.
	// Therefore, the blending process must be executed even if the blend weight is 0.
	if (!p_seek && p_optimize && !any_valid) {
		return p_node->_pre_process(new_path, new_parent, state, 0, p_seek, p_seek_root, p_connections);
	}
	return p_node->_pre_process(new_path, new_parent, state, p_time, p_seek, p_seek_root, p_connections);
}

int AnimationNode::get_input_count() const {
	return inputs.size();
}

String AnimationNode::get_input_name(int p_input) {
	ERR_FAIL_INDEX_V(p_input, inputs.size(), String());
	return inputs[p_input].name;
}

String AnimationNode::get_caption() const {
	String ret;
	if (GDVIRTUAL_CALL(_get_caption, ret)) {
		return ret;
	}

	return "Node";
}

void AnimationNode::add_input(const String &p_name) {
	//root nodes can't add inputs
	ERR_FAIL_COND(Object::cast_to<AnimationRootNode>(this) != nullptr);
	Input input;
	ERR_FAIL_COND(p_name.contains(".") || p_name.contains("/"));
	input.name = p_name;
	inputs.push_back(input);
	emit_changed();
}

void AnimationNode::set_input_name(int p_input, const String &p_name) {
	ERR_FAIL_INDEX(p_input, inputs.size());
	ERR_FAIL_COND(p_name.contains(".") || p_name.contains("/"));
	inputs.write[p_input].name = p_name;
	emit_changed();
}

void AnimationNode::remove_input(int p_index) {
	ERR_FAIL_INDEX(p_index, inputs.size());
	inputs.remove_at(p_index);
	emit_changed();
}

double AnimationNode::process(double p_time, bool p_seek, bool p_seek_root) {
	double ret;
	if (GDVIRTUAL_CALL(_process, p_time, p_seek, p_seek_root, ret)) {
		return ret;
	}

	return 0;
}

void AnimationNode::set_filter_path(const NodePath &p_path, bool p_enable) {
	if (p_enable) {
		filter[p_path] = true;
	} else {
		filter.erase(p_path);
	}
}

void AnimationNode::set_filter_enabled(bool p_enable) {
	filter_enabled = p_enable;
}

bool AnimationNode::is_filter_enabled() const {
	return filter_enabled;
}

bool AnimationNode::is_path_filtered(const NodePath &p_path) const {
	return filter.has(p_path);
}

bool AnimationNode::has_filter() const {
	bool ret;
	if (GDVIRTUAL_CALL(_has_filter, ret)) {
		return ret;
	}

	return false;
}

Array AnimationNode::_get_filters() const {
	Array paths;

	for (const KeyValue<NodePath, bool> &E : filter) {
		paths.push_back(String(E.key)); //use strings, so sorting is possible
	}
	paths.sort(); //done so every time the scene is saved, it does not change

	return paths;
}

void AnimationNode::_set_filters(const Array &p_filters) {
	filter.clear();
	for (int i = 0; i < p_filters.size(); i++) {
		set_filter_path(p_filters[i], true);
	}
}

void AnimationNode::_validate_property(PropertyInfo &property) const {
	if (!has_filter() && (property.name == "filter_enabled" || property.name == "filters")) {
		property.usage = PROPERTY_USAGE_NONE;
	}
}

Ref<AnimationNode> AnimationNode::get_child_by_name(const StringName &p_name) {
	Ref<AnimationNode> ret;
	if (GDVIRTUAL_CALL(_get_child_by_name, p_name, ret)) {
		return ret;
	}
	return Ref<AnimationNode>();
}

void AnimationNode::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_input_count"), &AnimationNode::get_input_count);
	ClassDB::bind_method(D_METHOD("get_input_name", "input"), &AnimationNode::get_input_name);

	ClassDB::bind_method(D_METHOD("add_input", "name"), &AnimationNode::add_input);
	ClassDB::bind_method(D_METHOD("remove_input", "index"), &AnimationNode::remove_input);

	ClassDB::bind_method(D_METHOD("set_filter_path", "path", "enable"), &AnimationNode::set_filter_path);
	ClassDB::bind_method(D_METHOD("is_path_filtered", "path"), &AnimationNode::is_path_filtered);

	ClassDB::bind_method(D_METHOD("set_filter_enabled", "enable"), &AnimationNode::set_filter_enabled);
	ClassDB::bind_method(D_METHOD("is_filter_enabled"), &AnimationNode::is_filter_enabled);

	ClassDB::bind_method(D_METHOD("_set_filters", "filters"), &AnimationNode::_set_filters);
	ClassDB::bind_method(D_METHOD("_get_filters"), &AnimationNode::_get_filters);

	ClassDB::bind_method(D_METHOD("blend_animation", "animation", "time", "delta", "seeked", "seek_root", "blend", "pingponged"), &AnimationNode::blend_animation, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("blend_node", "name", "node", "time", "seek", "seek_root", "blend", "filter", "optimize"), &AnimationNode::blend_node, DEFVAL(FILTER_IGNORE), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("blend_input", "input_index", "time", "seek", "seek_root", "blend", "filter", "optimize"), &AnimationNode::blend_input, DEFVAL(FILTER_IGNORE), DEFVAL(true));

	ClassDB::bind_method(D_METHOD("set_parameter", "name", "value"), &AnimationNode::set_parameter);
	ClassDB::bind_method(D_METHOD("get_parameter", "name"), &AnimationNode::get_parameter);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "filter_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_filter_enabled", "is_filter_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "filters", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_filters", "_get_filters");

	GDVIRTUAL_BIND(_get_child_nodes);
	GDVIRTUAL_BIND(_get_parameter_list);
	GDVIRTUAL_BIND(_get_child_by_name, "name");
	GDVIRTUAL_BIND(_get_parameter_default_value, "parameter");
	GDVIRTUAL_BIND(_process, "time", "seek", "seek_root");
	GDVIRTUAL_BIND(_get_caption);
	GDVIRTUAL_BIND(_has_filter);

	ADD_SIGNAL(MethodInfo("removed_from_graph"));

	ADD_SIGNAL(MethodInfo("tree_changed"));

	BIND_ENUM_CONSTANT(FILTER_IGNORE);
	BIND_ENUM_CONSTANT(FILTER_PASS);
	BIND_ENUM_CONSTANT(FILTER_STOP);
	BIND_ENUM_CONSTANT(FILTER_BLEND);
}

AnimationNode::AnimationNode() {
}

////////////////////

void AnimationTree::set_tree_root(const Ref<AnimationNode> &p_root) {
	if (root.is_valid()) {
		root->disconnect("tree_changed", callable_mp(this, &AnimationTree::_tree_changed));
	}

	root = p_root;

	if (root.is_valid()) {
		root->connect("tree_changed", callable_mp(this, &AnimationTree::_tree_changed));
	}

	properties_dirty = true;

	update_configuration_warnings();
}

Ref<AnimationNode> AnimationTree::get_tree_root() const {
	return root;
}

void AnimationTree::set_active(bool p_active) {
	if (active == p_active) {
		return;
	}

	active = p_active;
	started = active;

	if (process_callback == ANIMATION_PROCESS_IDLE) {
		set_process_internal(active);
	} else {
		set_physics_process_internal(active);
	}

	if (!active && is_inside_tree()) {
		for (const TrackCache *E : playing_caches) {
			if (ObjectDB::get_instance(E->object_id)) {
				E->object->call(SNAME("stop"));
			}
		}

		playing_caches.clear();
	}
}

bool AnimationTree::is_active() const {
	return active;
}

void AnimationTree::set_process_callback(AnimationProcessCallback p_mode) {
	if (process_callback == p_mode) {
		return;
	}

	bool was_active = is_active();
	if (was_active) {
		set_active(false);
	}

	process_callback = p_mode;

	if (was_active) {
		set_active(true);
	}
}

AnimationTree::AnimationProcessCallback AnimationTree::get_process_callback() const {
	return process_callback;
}

void AnimationTree::_node_removed(Node *p_node) {
	cache_valid = false;
}

bool AnimationTree::_update_caches(AnimationPlayer *player) {
	setup_pass++;

	if (!player->has_node(player->get_root())) {
		ERR_PRINT("AnimationTree: AnimationPlayer root is invalid.");
		set_active(false);
		return false;
	}
	Node *parent = player->get_node(player->get_root());

	List<StringName> sname;
	player->get_animation_list(&sname);

	Ref<Animation> reset_anim;
	bool has_reset_anim = player->has_animation(SceneStringNames::get_singleton()->RESET);
	if (has_reset_anim) {
		reset_anim = player->get_animation(SceneStringNames::get_singleton()->RESET);
	}
	for (const StringName &E : sname) {
		Ref<Animation> anim = player->get_animation(E);
		for (int i = 0; i < anim->get_track_count(); i++) {
			NodePath path = anim->track_get_path(i);
			Animation::TrackType track_type = anim->track_get_type(i);

			Animation::TrackType track_cache_type = track_type;
			if (track_cache_type == Animation::TYPE_POSITION_3D || track_cache_type == Animation::TYPE_ROTATION_3D || track_cache_type == Animation::TYPE_SCALE_3D) {
				track_cache_type = Animation::TYPE_POSITION_3D; //reference them as position3D tracks, even if they modify rotation or scale
			}

			TrackCache *track = nullptr;
			if (track_cache.has(path)) {
				track = track_cache.get(path);
			}

			//if not valid, delete track
			if (track && (track->type != track_cache_type || ObjectDB::get_instance(track->object_id) == nullptr)) {
				playing_caches.erase(track);
				memdelete(track);
				track_cache.erase(path);
				track = nullptr;
			}

			if (!track) {
				Ref<Resource> resource;
				Vector<StringName> leftover_path;
				Node *child = parent->get_node_and_resource(path, resource, leftover_path);

				if (!child) {
					ERR_PRINT("AnimationTree: '" + String(E) + "', couldn't resolve track:  '" + String(path) + "'");
					continue;
				}

				if (!child->is_connected("tree_exited", callable_mp(this, &AnimationTree::_node_removed))) {
					child->connect("tree_exited", callable_mp(this, &AnimationTree::_node_removed), varray(child));
				}

				switch (track_type) {
					case Animation::TYPE_VALUE: {
						TrackCacheValue *track_value = memnew(TrackCacheValue);

						if (resource.is_valid()) {
							track_value->object = resource.ptr();
						} else {
							track_value->object = child;
						}

						track_value->subpath = leftover_path;
						track_value->object_id = track_value->object->get_instance_id();

						track = track_value;

						if (has_reset_anim) {
							int rt = reset_anim->find_track(path, track_type);
							if (rt >= 0 && reset_anim->track_get_key_count(rt) > 0) {
								track_value->init_value = reset_anim->track_get_key_value(rt, 0);
							}
						}
					} break;
					case Animation::TYPE_POSITION_3D:
					case Animation::TYPE_ROTATION_3D:
					case Animation::TYPE_SCALE_3D: {
#ifndef _3D_DISABLED
						Node3D *node_3d = Object::cast_to<Node3D>(child);

						if (!node_3d) {
							ERR_PRINT("AnimationTree: '" + String(E) + "', transform track does not point to Node3D:  '" + String(path) + "'");
							continue;
						}

						TrackCacheTransform *track_xform = memnew(TrackCacheTransform);
						track_xform->type = Animation::TYPE_POSITION_3D;

						track_xform->node_3d = node_3d;
						track_xform->skeleton = nullptr;
						track_xform->bone_idx = -1;

						bool has_rest = false;
						if (path.get_subname_count() == 1 && Object::cast_to<Skeleton3D>(node_3d)) {
							Skeleton3D *sk = Object::cast_to<Skeleton3D>(node_3d);
							track_xform->skeleton = sk;
							int bone_idx = sk->find_bone(path.get_subname(0));
							if (bone_idx != -1) {
								has_rest = true;
								track_xform->bone_idx = bone_idx;
								Transform3D rest = sk->get_bone_rest(bone_idx);
								track_xform->init_loc = rest.origin;
								track_xform->init_rot = rest.basis.get_rotation_quaternion();
								track_xform->init_scale = rest.basis.get_scale();
							}
						}

						track_xform->object = node_3d;
						track_xform->object_id = track_xform->object->get_instance_id();

						track = track_xform;

						switch (track_type) {
							case Animation::TYPE_POSITION_3D: {
								track_xform->loc_used = true;
							} break;
							case Animation::TYPE_ROTATION_3D: {
								track_xform->rot_used = true;
							} break;
							case Animation::TYPE_SCALE_3D: {
								track_xform->scale_used = true;
							} break;
							default: {
							}
						}

						// For non Skeleton3D bone animation.
						if (has_reset_anim && !has_rest) {
							int rt = reset_anim->find_track(path, track_type);
							if (rt >= 0 && reset_anim->track_get_key_count(rt) > 0) {
								switch (track_type) {
									case Animation::TYPE_POSITION_3D: {
										track_xform->init_loc = reset_anim->track_get_key_value(rt, 0);
									} break;
									case Animation::TYPE_ROTATION_3D: {
										track_xform->init_rot = reset_anim->track_get_key_value(rt, 0);
									} break;
									case Animation::TYPE_SCALE_3D: {
										track_xform->init_scale = reset_anim->track_get_key_value(rt, 0);
									} break;
									default: {
									}
								}
							}
						}
#endif // _3D_DISABLED
					} break;
					case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
						if (path.get_subname_count() != 1) {
							ERR_PRINT("AnimationTree: '" + String(E) + "', blend shape track does not contain a blend shape subname:  '" + String(path) + "'");
							continue;
						}
						MeshInstance3D *mesh_3d = Object::cast_to<MeshInstance3D>(child);

						if (!mesh_3d) {
							ERR_PRINT("AnimationTree: '" + String(E) + "', blend shape track does not point to MeshInstance3D:  '" + String(path) + "'");
							continue;
						}

						StringName blend_shape_name = path.get_subname(0);
						int blend_shape_idx = mesh_3d->find_blend_shape_by_name(blend_shape_name);
						if (blend_shape_idx == -1) {
							ERR_PRINT("AnimationTree: '" + String(E) + "', blend shape track points to a non-existing name:  '" + String(blend_shape_name) + "'");
							continue;
						}

						TrackCacheBlendShape *track_bshape = memnew(TrackCacheBlendShape);

						track_bshape->mesh_3d = mesh_3d;
						track_bshape->shape_index = blend_shape_idx;

						track_bshape->object = mesh_3d;
						track_bshape->object_id = mesh_3d->get_instance_id();
						track = track_bshape;

						if (has_reset_anim) {
							int rt = reset_anim->find_track(path, track_type);
							if (rt >= 0 && reset_anim->track_get_key_count(rt) > 0) {
								track_bshape->init_value = reset_anim->track_get_key_value(rt, 0);
							}
						}
#endif
					} break;
					case Animation::TYPE_METHOD: {
						TrackCacheMethod *track_method = memnew(TrackCacheMethod);

						if (resource.is_valid()) {
							track_method->object = resource.ptr();
						} else {
							track_method->object = child;
						}

						track_method->object_id = track_method->object->get_instance_id();

						track = track_method;

					} break;
					case Animation::TYPE_BEZIER: {
						TrackCacheBezier *track_bezier = memnew(TrackCacheBezier);

						if (resource.is_valid()) {
							track_bezier->object = resource.ptr();
						} else {
							track_bezier->object = child;
						}

						track_bezier->subpath = leftover_path;
						track_bezier->object_id = track_bezier->object->get_instance_id();

						track = track_bezier;

						if (has_reset_anim) {
							int rt = reset_anim->find_track(path, track_type);
							if (rt >= 0 && reset_anim->track_get_key_count(rt) > 0) {
								track_bezier->init_value = reset_anim->track_get_key_value(rt, 0);
							}
						}
					} break;
					case Animation::TYPE_AUDIO: {
						TrackCacheAudio *track_audio = memnew(TrackCacheAudio);

						track_audio->object = child;
						track_audio->object_id = track_audio->object->get_instance_id();

						track = track_audio;

					} break;
					case Animation::TYPE_ANIMATION: {
						TrackCacheAnimation *track_animation = memnew(TrackCacheAnimation);

						track_animation->object = child;
						track_animation->object_id = track_animation->object->get_instance_id();

						track = track_animation;

					} break;
					default: {
						ERR_PRINT("Animation corrupted (invalid track type)");
						continue;
					}
				}

				track_cache[path] = track;
			} else if (track_cache_type == Animation::TYPE_POSITION_3D) {
				TrackCacheTransform *track_xform = static_cast<TrackCacheTransform *>(track);
				if (track->setup_pass != setup_pass) {
					track_xform->loc_used = false;
					track_xform->rot_used = false;
					track_xform->scale_used = false;
				}
				switch (track_type) {
					case Animation::TYPE_POSITION_3D: {
						track_xform->loc_used = true;
					} break;
					case Animation::TYPE_ROTATION_3D: {
						track_xform->rot_used = true;
					} break;
					case Animation::TYPE_SCALE_3D: {
						track_xform->scale_used = true;
					} break;
					default: {
					}
				}
			}

			track->setup_pass = setup_pass;
		}
	}

	List<NodePath> to_delete;

	for (const KeyValue<NodePath, TrackCache *> &K : track_cache) {
		TrackCache *tc = track_cache[K.key];
		if (tc->setup_pass != setup_pass) {
			to_delete.push_back(K.key);
		}
	}

	while (to_delete.front()) {
		NodePath np = to_delete.front()->get();
		memdelete(track_cache[np]);
		track_cache.erase(np);
		to_delete.pop_front();
	}

	state.track_map.clear();

	int idx = 0;
	for (const KeyValue<NodePath, TrackCache *> &K : track_cache) {
		state.track_map[K.key] = idx;
		idx++;
	}

	state.track_count = idx;

	cache_valid = true;

	return true;
}

void AnimationTree::_clear_caches() {
	for (KeyValue<NodePath, TrackCache *> &K : track_cache) {
		memdelete(K.value);
	}
	playing_caches.clear();

	track_cache.clear();
	cache_valid = false;
}

static void _call_object(Object *p_object, const StringName &p_method, const Vector<Variant> &p_params, bool p_deferred) {
	// Separate function to use alloca() more efficiently
	const Variant **argptrs = (const Variant **)alloca(sizeof(const Variant **) * p_params.size());
	const Variant *args = p_params.ptr();
	uint32_t argcount = p_params.size();
	for (uint32_t i = 0; i < argcount; i++) {
		argptrs[i] = &args[i];
	}
	if (p_deferred) {
		MessageQueue::get_singleton()->push_callp(p_object, p_method, argptrs, argcount);
	} else {
		Callable::CallError ce;
		p_object->callp(p_method, argptrs, argcount, ce);
	}
}
void AnimationTree::_process_graph(double p_delta) {
	_update_properties(); //if properties need updating, update them

	//check all tracks, see if they need modification
	root_motion_transform = Transform3D();

	if (!root.is_valid()) {
		ERR_PRINT("AnimationTree: root AnimationNode is not set, disabling playback.");
		set_active(false);
		cache_valid = false;
		return;
	}

	if (!has_node(animation_player)) {
		ERR_PRINT("AnimationTree: no valid AnimationPlayer path set, disabling playback");
		set_active(false);
		cache_valid = false;
		return;
	}

	AnimationPlayer *player = Object::cast_to<AnimationPlayer>(get_node(animation_player));

	ObjectID current_animation_player;

	if (player) {
		current_animation_player = player->get_instance_id();
	}

	if (last_animation_player != current_animation_player) {
		if (last_animation_player.is_valid()) {
			Object *old_player = ObjectDB::get_instance(last_animation_player);
			if (old_player) {
				old_player->disconnect("caches_cleared", callable_mp(this, &AnimationTree::_clear_caches));
			}
		}

		if (player) {
			player->connect("caches_cleared", callable_mp(this, &AnimationTree::_clear_caches));
		}

		last_animation_player = current_animation_player;
	}

	if (!player) {
		ERR_PRINT("AnimationTree: path points to a node not an AnimationPlayer, disabling playback");
		set_active(false);
		cache_valid = false;
		return;
	}

	if (!cache_valid) {
		if (!_update_caches(player)) {
			return;
		}
	}

	{ //setup

		process_pass++;

		state.valid = true;
		state.invalid_reasons = "";
		state.animation_states.clear(); //will need to be re-created
		state.player = player;
		state.last_pass = process_pass;
		state.tree = this;

		// root source blends

		root->blends.resize(state.track_count);
		real_t *src_blendsw = root->blends.ptrw();
		for (int i = 0; i < state.track_count; i++) {
			src_blendsw[i] = 1.0; //by default all go to 1 for the root input
		}
	}

	//process

	{
		if (started) {
			//if started, seek
			root->_pre_process(SceneStringNames::get_singleton()->parameters_base_path, nullptr, &state, 0, true, false, Vector<StringName>());
			started = false;
		}

		root->_pre_process(SceneStringNames::get_singleton()->parameters_base_path, nullptr, &state, p_delta, false, false, Vector<StringName>());
	}

	if (!state.valid) {
		return; //state is not valid. do nothing.
	}
	//apply value/transform/bezier blends to track caches and execute method/audio/animation tracks

	{
		bool can_call = is_inside_tree() && !Engine::get_singleton()->is_editor_hint();

		for (const AnimationNode::AnimationState &as : state.animation_states) {
			Ref<Animation> a = as.animation;
			double time = as.time;
			double delta = as.delta;
			real_t weight = as.blend;
			bool seeked = as.seeked;
			int pingponged = as.pingponged;
#ifndef _3D_DISABLED
			bool backward = signbit(delta);
			bool calc_root = !seeked || as.seek_root;
#endif // _3D_DISABLED

			for (int i = 0; i < a->get_track_count(); i++) {
				if (!a->track_is_enabled(i)) {
					continue;
				}

				NodePath path = a->track_get_path(i);

				ERR_CONTINUE(!track_cache.has(path));

				TrackCache *track = track_cache[path];

				Animation::TrackType ttype = a->track_get_type(i);
				if (ttype != Animation::TYPE_POSITION_3D && ttype != Animation::TYPE_ROTATION_3D && ttype != Animation::TYPE_SCALE_3D && track->type != ttype) {
					//broken animation, but avoid error spamming
					continue;
				}

				track->root_motion = root_motion_track == path;

				ERR_CONTINUE(!state.track_map.has(path));
				int blend_idx = state.track_map[path];

				ERR_CONTINUE(blend_idx < 0 || blend_idx >= state.track_count);

				real_t blend = (*as.track_blends)[blend_idx] * weight;

				switch (ttype) {
					case Animation::TYPE_POSITION_3D: {
#ifndef _3D_DISABLED
						TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
						if (track->root_motion && calc_root) {
							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								t->loc = Vector3(0, 0, 0);
								t->rot = Quaternion(0, 0, 0, 1);
								t->scale = Vector3(0, 0, 0);
							}
							double prev_time = time - delta;
							if (!backward) {
								if (prev_time < 0) {
									switch (a->get_loop_mode()) {
										case Animation::LOOP_NONE: {
											prev_time = 0;
										} break;
										case Animation::LOOP_LINEAR: {
											prev_time = Math::fposmod(prev_time, (double)a->get_length());
										} break;
										case Animation::LOOP_PINGPONG: {
											prev_time = Math::pingpong(prev_time, (double)a->get_length());
										} break;
										default:
											break;
									}
								}
							} else {
								if (prev_time > a->get_length()) {
									switch (a->get_loop_mode()) {
										case Animation::LOOP_NONE: {
											prev_time = (double)a->get_length();
										} break;
										case Animation::LOOP_LINEAR: {
											prev_time = Math::fposmod(prev_time, (double)a->get_length());
										} break;
										case Animation::LOOP_PINGPONG: {
											prev_time = Math::pingpong(prev_time, (double)a->get_length());
										} break;
										default:
											break;
									}
								}
							}

							Vector3 loc[2];

							if (!backward) {
								if (prev_time > time) {
									Error err = a->position_track_interpolate(i, prev_time, &loc[0]);
									if (err != OK) {
										continue;
									}
									a->position_track_interpolate(i, (double)a->get_length(), &loc[1]);
									t->loc += (loc[1] - loc[0]) * blend;
									prev_time = 0;
								}
							} else {
								if (prev_time < time) {
									Error err = a->position_track_interpolate(i, prev_time, &loc[0]);
									if (err != OK) {
										continue;
									}
									a->position_track_interpolate(i, 0, &loc[1]);
									t->loc += (loc[1] - loc[0]) * blend;
									prev_time = (double)a->get_length();
								}
							}

							Error err = a->position_track_interpolate(i, prev_time, &loc[0]);
							if (err != OK) {
								continue;
							}

							a->position_track_interpolate(i, time, &loc[1]);
							t->loc += (loc[1] - loc[0]) * blend;
							prev_time = !backward ? 0 : (double)a->get_length();

						} else {
							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								t->loc = t->init_loc;
								t->rot = t->init_rot;
								t->scale = t->init_scale;
							}
							Vector3 loc;

							Error err = a->position_track_interpolate(i, time, &loc);
							if (err != OK) {
								continue;
							}

							t->loc += (loc - t->init_loc) * blend;
						}
#endif // _3D_DISABLED
					} break;
					case Animation::TYPE_ROTATION_3D: {
#ifndef _3D_DISABLED
						TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
						if (track->root_motion && calc_root) {
							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								t->loc = Vector3(0, 0, 0);
								t->rot = Quaternion(0, 0, 0, 1);
								t->scale = Vector3(0, 0, 0);
							}
							double prev_time = time - delta;
							if (!backward) {
								if (prev_time < 0) {
									switch (a->get_loop_mode()) {
										case Animation::LOOP_NONE: {
											prev_time = 0;
										} break;
										case Animation::LOOP_LINEAR: {
											prev_time = Math::fposmod(prev_time, (double)a->get_length());
										} break;
										case Animation::LOOP_PINGPONG: {
											prev_time = Math::pingpong(prev_time, (double)a->get_length());
										} break;
										default:
											break;
									}
								}
							} else {
								if (prev_time > a->get_length()) {
									switch (a->get_loop_mode()) {
										case Animation::LOOP_NONE: {
											prev_time = (double)a->get_length();
										} break;
										case Animation::LOOP_LINEAR: {
											prev_time = Math::fposmod(prev_time, (double)a->get_length());
										} break;
										case Animation::LOOP_PINGPONG: {
											prev_time = Math::pingpong(prev_time, (double)a->get_length());
										} break;
										default:
											break;
									}
								}
							}

							Quaternion rot[2];

							if (!backward) {
								if (prev_time > time) {
									Error err = a->rotation_track_interpolate(i, prev_time, &rot[0]);
									if (err != OK) {
										continue;
									}
									a->rotation_track_interpolate(i, (double)a->get_length(), &rot[1]);
									t->rot = (t->rot * Quaternion().slerp(rot[0].inverse() * rot[1], blend)).normalized();
									prev_time = 0;
								}
							} else {
								if (prev_time < time) {
									Error err = a->rotation_track_interpolate(i, prev_time, &rot[0]);
									if (err != OK) {
										continue;
									}
									a->rotation_track_interpolate(i, 0, &rot[1]);
									t->rot = (t->rot * Quaternion().slerp(rot[0].inverse() * rot[1], blend)).normalized();
									prev_time = (double)a->get_length();
								}
							}

							Error err = a->rotation_track_interpolate(i, prev_time, &rot[0]);
							if (err != OK) {
								continue;
							}

							a->rotation_track_interpolate(i, time, &rot[1]);
							t->rot = (t->rot * Quaternion().slerp(rot[0].inverse() * rot[1], blend)).normalized();
							prev_time = !backward ? 0 : (double)a->get_length();

						} else {
							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								t->loc = t->init_loc;
								t->rot = t->init_rot;
								t->scale = t->init_scale;
							}
							Quaternion rot;

							Error err = a->rotation_track_interpolate(i, time, &rot);
							if (err != OK) {
								continue;
							}

							t->rot = (t->rot * Quaternion().slerp(t->init_rot.inverse() * rot, blend)).normalized();
						}
#endif // _3D_DISABLED
					} break;
					case Animation::TYPE_SCALE_3D: {
#ifndef _3D_DISABLED
						TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
						if (track->root_motion && calc_root) {
							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								t->loc = Vector3(0, 0, 0);
								t->rot = Quaternion(0, 0, 0, 1);
								t->scale = Vector3(0, 0, 0);
							}
							double prev_time = time - delta;
							if (!backward) {
								if (prev_time < 0) {
									switch (a->get_loop_mode()) {
										case Animation::LOOP_NONE: {
											prev_time = 0;
										} break;
										case Animation::LOOP_LINEAR: {
											prev_time = Math::fposmod(prev_time, (double)a->get_length());
										} break;
										case Animation::LOOP_PINGPONG: {
											prev_time = Math::pingpong(prev_time, (double)a->get_length());
										} break;
										default:
											break;
									}
								}
							} else {
								if (prev_time > a->get_length()) {
									switch (a->get_loop_mode()) {
										case Animation::LOOP_NONE: {
											prev_time = (double)a->get_length();
										} break;
										case Animation::LOOP_LINEAR: {
											prev_time = Math::fposmod(prev_time, (double)a->get_length());
										} break;
										case Animation::LOOP_PINGPONG: {
											prev_time = Math::pingpong(prev_time, (double)a->get_length());
										} break;
										default:
											break;
									}
								}
							}

							Vector3 scale[2];

							if (!backward) {
								if (prev_time > time) {
									Error err = a->scale_track_interpolate(i, prev_time, &scale[0]);
									if (err != OK) {
										continue;
									}
									a->scale_track_interpolate(i, (double)a->get_length(), &scale[1]);
									t->scale += (scale[1] - scale[0]) * blend;
									prev_time = 0;
								}
							} else {
								if (prev_time < time) {
									Error err = a->scale_track_interpolate(i, prev_time, &scale[0]);
									if (err != OK) {
										continue;
									}
									a->scale_track_interpolate(i, 0, &scale[1]);
									t->scale += (scale[1] - scale[0]) * blend;
									prev_time = (double)a->get_length();
								}
							}

							Error err = a->scale_track_interpolate(i, prev_time, &scale[0]);
							if (err != OK) {
								continue;
							}

							a->scale_track_interpolate(i, time, &scale[1]);
							t->scale += (scale[1] - scale[0]) * blend;
							prev_time = !backward ? 0 : (double)a->get_length();

						} else {
							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								t->loc = t->init_loc;
								t->rot = t->init_rot;
								t->scale = t->init_scale;
							}
							Vector3 scale;

							Error err = a->scale_track_interpolate(i, time, &scale);
							if (err != OK) {
								continue;
							}

							t->scale += (scale - t->init_scale) * blend;
						}
#endif // _3D_DISABLED
					} break;
					case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
						TrackCacheBlendShape *t = static_cast<TrackCacheBlendShape *>(track);

						if (t->process_pass != process_pass) {
							t->process_pass = process_pass;
							t->value = t->init_value;
						}

						float value;

						Error err = a->blend_shape_track_interpolate(i, time, &value);
						//ERR_CONTINUE(err!=OK); //used for testing, should be removed

						if (err != OK) {
							continue;
						}

						t->value += (value - t->init_value) * blend;
#endif // _3D_DISABLED
					} break;
					case Animation::TYPE_VALUE: {
						TrackCacheValue *t = static_cast<TrackCacheValue *>(track);

						Animation::UpdateMode update_mode = a->value_track_get_update_mode(i);

						if (update_mode == Animation::UPDATE_CONTINUOUS || update_mode == Animation::UPDATE_CAPTURE) {
							Variant value = a->value_track_interpolate(i, time);

							if (value == Variant()) {
								continue;
							}

							if (t->process_pass != process_pass) {
								t->process_pass = process_pass;
								if (!t->init_value) {
									t->init_value = value;
									t->init_value.zero();
								}
								t->value = t->init_value;
							}

							Variant::sub(value, t->init_value, value);
							Variant::blend(t->value, value, blend, t->value);
						} else {
							if (blend < CMP_EPSILON) {
								continue; //nothing to blend
							}

							if (seeked) {
								int idx = a->track_find_key(i, time);
								if (idx < 0) {
									continue;
								}
								Variant value = a->track_get_key_value(i, idx);
								t->object->set_indexed(t->subpath, value);
							} else {
								List<int> indices;
								a->value_track_get_key_indices(i, time, delta, &indices, pingponged);
								for (int &F : indices) {
									Variant value = a->track_get_key_value(i, F);
									t->object->set_indexed(t->subpath, value);
								}
							}
						}

					} break;
					case Animation::TYPE_METHOD: {
						if (blend < CMP_EPSILON) {
							continue; //nothing to blend
						}
						TrackCacheMethod *t = static_cast<TrackCacheMethod *>(track);

						if (seeked) {
							int idx = a->track_find_key(i, time);
							if (idx < 0) {
								continue;
							}
							StringName method = a->method_track_get_name(i, idx);
							Vector<Variant> params = a->method_track_get_params(i, idx);
							if (can_call) {
								_call_object(t->object, method, params, false);
							}
						} else {
							List<int> indices;
							a->method_track_get_key_indices(i, time, delta, &indices, pingponged);
							for (int &F : indices) {
								StringName method = a->method_track_get_name(i, F);
								Vector<Variant> params = a->method_track_get_params(i, F);
								if (can_call) {
									_call_object(t->object, method, params, true);
								}
							}
						}
					} break;
					case Animation::TYPE_BEZIER: {
						TrackCacheBezier *t = static_cast<TrackCacheBezier *>(track);

						real_t bezier = a->bezier_track_interpolate(i, time);

						if (t->process_pass != process_pass) {
							t->process_pass = process_pass;
							t->value = t->init_value;
						}

						t->value += (bezier - t->init_value) * blend;
					} break;
					case Animation::TYPE_AUDIO: {
						if (blend < CMP_EPSILON) {
							continue; //nothing to blend
						}
						TrackCacheAudio *t = static_cast<TrackCacheAudio *>(track);

						if (seeked) {
							//find whatever should be playing
							int idx = a->track_find_key(i, time);
							if (idx < 0) {
								continue;
							}

							Ref<AudioStream> stream = a->audio_track_get_key_stream(i, idx);
							if (!stream.is_valid()) {
								t->object->call(SNAME("stop"));
								t->playing = false;
								playing_caches.erase(t);
							} else {
								double start_ofs = a->audio_track_get_key_start_offset(i, idx);
								start_ofs += time - a->track_get_key_time(i, idx);
								double end_ofs = a->audio_track_get_key_end_offset(i, idx);
								double len = stream->get_length();

								if (start_ofs > len - end_ofs) {
									t->object->call(SNAME("stop"));
									t->playing = false;
									playing_caches.erase(t);
									continue;
								}

								t->object->call(SNAME("set_stream"), stream);
								t->object->call(SNAME("play"), start_ofs);

								t->playing = true;
								playing_caches.insert(t);
								if (len && end_ofs > 0) { //force an end at a time
									t->len = len - start_ofs - end_ofs;
								} else {
									t->len = 0;
								}

								t->start = time;
							}

						} else {
							//find stuff to play
							List<int> to_play;
							a->track_get_key_indices_in_range(i, time, delta, &to_play, pingponged);
							if (to_play.size()) {
								int idx = to_play.back()->get();

								Ref<AudioStream> stream = a->audio_track_get_key_stream(i, idx);
								if (!stream.is_valid()) {
									t->object->call(SNAME("stop"));
									t->playing = false;
									playing_caches.erase(t);
								} else {
									double start_ofs = a->audio_track_get_key_start_offset(i, idx);
									double end_ofs = a->audio_track_get_key_end_offset(i, idx);
									double len = stream->get_length();

									t->object->call(SNAME("set_stream"), stream);
									t->object->call(SNAME("play"), start_ofs);

									t->playing = true;
									playing_caches.insert(t);
									if (len && end_ofs > 0) { //force an end at a time
										t->len = len - start_ofs - end_ofs;
									} else {
										t->len = 0;
									}

									t->start = time;
								}
							} else if (t->playing) {
								bool loop = a->get_loop_mode() != Animation::LOOP_NONE;

								bool stop = false;

								if (!loop) {
									if (delta > 0) {
										if (time < t->start) {
											stop = true;
										}
									} else if (delta < 0) {
										if (time > t->start) {
											stop = true;
										}
									}
								} else if (t->len > 0) {
									double len = t->start > time ? (a->get_length() - t->start) + time : time - t->start;

									if (len > t->len) {
										stop = true;
									}
								}

								if (stop) {
									//time to stop
									t->object->call(SNAME("stop"));
									t->playing = false;
									playing_caches.erase(t);
								}
							}
						}

						real_t db = Math::linear2db(MAX(blend, 0.00001));
						if (t->object->has_method(SNAME("set_unit_db"))) {
							t->object->call(SNAME("set_unit_db"), db);
						} else {
							t->object->call(SNAME("set_volume_db"), db);
						}
					} break;
					case Animation::TYPE_ANIMATION: {
						if (blend < CMP_EPSILON) {
							continue; //nothing to blend
						}
						TrackCacheAnimation *t = static_cast<TrackCacheAnimation *>(track);

						AnimationPlayer *player2 = Object::cast_to<AnimationPlayer>(t->object);

						if (!player2) {
							continue;
						}

						if (seeked) {
							//seek
							int idx = a->track_find_key(i, time);
							if (idx < 0) {
								continue;
							}

							double pos = a->track_get_key_time(i, idx);

							StringName anim_name = a->animation_track_get_key_animation(i, idx);
							if (String(anim_name) == "[stop]" || !player2->has_animation(anim_name)) {
								continue;
							}

							Ref<Animation> anim = player2->get_animation(anim_name);

							double at_anim_pos = 0.0;

							switch (anim->get_loop_mode()) {
								case Animation::LOOP_NONE: {
									at_anim_pos = MAX((double)anim->get_length(), time - pos); //seek to end
								} break;
								case Animation::LOOP_LINEAR: {
									at_anim_pos = Math::fposmod(time - pos, (double)anim->get_length()); //seek to loop
								} break;
								case Animation::LOOP_PINGPONG: {
									at_anim_pos = Math::pingpong(time - pos, (double)a->get_length());
								} break;
								default:
									break;
							}

							if (player2->is_playing() || seeked) {
								player2->play(anim_name);
								player2->seek(at_anim_pos);
								t->playing = true;
								playing_caches.insert(t);
							} else {
								player2->set_assigned_animation(anim_name);
								player2->seek(at_anim_pos, true);
							}
						} else {
							//find stuff to play
							List<int> to_play;
							a->track_get_key_indices_in_range(i, time, delta, &to_play, pingponged);
							if (to_play.size()) {
								int idx = to_play.back()->get();

								StringName anim_name = a->animation_track_get_key_animation(i, idx);
								if (String(anim_name) == "[stop]" || !player2->has_animation(anim_name)) {
									if (playing_caches.has(t)) {
										playing_caches.erase(t);
										player2->stop();
										t->playing = false;
									}
								} else {
									player2->play(anim_name);
									t->playing = true;
									playing_caches.insert(t);
								}
							}
						}

					} break;
				}
			}
		}
	}

	{
		// finally, set the tracks
		for (const KeyValue<NodePath, TrackCache *> &K : track_cache) {
			TrackCache *track = K.value;
			if (track->process_pass != process_pass) {
				continue; //not processed, ignore
			}

			switch (track->type) {
				case Animation::TYPE_POSITION_3D: {
#ifndef _3D_DISABLED
					TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);

					if (t->root_motion) {
						Transform3D xform;
						xform.origin = t->loc;
						xform.basis.set_quaternion_scale(t->rot, Vector3(1, 1, 1) + t->scale);

						root_motion_transform = xform;

					} else if (t->skeleton && t->bone_idx >= 0) {
						if (t->loc_used) {
							t->skeleton->set_bone_pose_position(t->bone_idx, t->loc);
						}
						if (t->rot_used) {
							t->skeleton->set_bone_pose_rotation(t->bone_idx, t->rot);
						}
						if (t->scale_used) {
							t->skeleton->set_bone_pose_scale(t->bone_idx, t->scale);
						}

					} else if (!t->skeleton) {
						if (t->loc_used) {
							t->node_3d->set_position(t->loc);
						}
						if (t->rot_used) {
							t->node_3d->set_rotation(t->rot.get_euler());
						}
						if (t->scale_used) {
							t->node_3d->set_scale(t->scale);
						}
					}
#endif // _3D_DISABLED
				} break;
				case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
					TrackCacheBlendShape *t = static_cast<TrackCacheBlendShape *>(track);

					if (t->mesh_3d) {
						t->mesh_3d->set_blend_shape_value(t->shape_index, t->value);
					}
#endif // _3D_DISABLED
				} break;
				case Animation::TYPE_VALUE: {
					TrackCacheValue *t = static_cast<TrackCacheValue *>(track);

					t->object->set_indexed(t->subpath, t->value);

				} break;
				case Animation::TYPE_BEZIER: {
					TrackCacheBezier *t = static_cast<TrackCacheBezier *>(track);

					t->object->set_indexed(t->subpath, t->value);

				} break;
				default: {
				} //the rest don't matter
			}
		}
	}
}

void AnimationTree::advance(real_t p_time) {
	_process_graph(p_time);
}

void AnimationTree::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (last_animation_player.is_valid()) {
				Object *player = ObjectDB::get_instance(last_animation_player);
				if (player) {
					player->connect("caches_cleared", callable_mp(this, &AnimationTree::_clear_caches));
				}
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_clear_caches();
			if (last_animation_player.is_valid()) {
				Object *player = ObjectDB::get_instance(last_animation_player);
				if (player) {
					player->disconnect("caches_cleared", callable_mp(this, &AnimationTree::_clear_caches));
				}
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (active && process_callback == ANIMATION_PROCESS_IDLE) {
				_process_graph(get_process_delta_time());
			}
		} break;

		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (active && process_callback == ANIMATION_PROCESS_PHYSICS) {
				_process_graph(get_physics_process_delta_time());
			}
		} break;
	}
}

void AnimationTree::set_animation_player(const NodePath &p_player) {
	animation_player = p_player;
	update_configuration_warnings();
}

NodePath AnimationTree::get_animation_player() const {
	return animation_player;
}

void AnimationTree::set_advance_expression_base_node(const NodePath &p_advance_expression_base_node) {
	advance_expression_base_node = p_advance_expression_base_node;
}

NodePath AnimationTree::get_advance_expression_base_node() const {
	return advance_expression_base_node;
}

bool AnimationTree::is_state_invalid() const {
	return !state.valid;
}

String AnimationTree::get_invalid_state_reason() const {
	return state.invalid_reasons;
}

uint64_t AnimationTree::get_last_process_pass() const {
	return process_pass;
}

TypedArray<String> AnimationTree::get_configuration_warnings() const {
	TypedArray<String> warnings = Node::get_configuration_warnings();

	if (!root.is_valid()) {
		warnings.push_back(RTR("No root AnimationNode for the graph is set."));
	}

	if (!has_node(animation_player)) {
		warnings.push_back(RTR("Path to an AnimationPlayer node containing animations is not set."));
	} else {
		AnimationPlayer *player = Object::cast_to<AnimationPlayer>(get_node(animation_player));

		if (!player) {
			warnings.push_back(RTR("Path set for AnimationPlayer does not lead to an AnimationPlayer node."));
		} else if (!player->has_node(player->get_root())) {
			warnings.push_back(RTR("The AnimationPlayer root node is not a valid node."));
		}
	}

	return warnings;
}

void AnimationTree::set_root_motion_track(const NodePath &p_track) {
	root_motion_track = p_track;
}

NodePath AnimationTree::get_root_motion_track() const {
	return root_motion_track;
}

Transform3D AnimationTree::get_root_motion_transform() const {
	return root_motion_transform;
}

void AnimationTree::_tree_changed() {
	if (properties_dirty) {
		return;
	}

	call_deferred(SNAME("_update_properties"));
	properties_dirty = true;
}

void AnimationTree::_update_properties_for_node(const String &p_base_path, Ref<AnimationNode> node) {
	ERR_FAIL_COND(node.is_null());
	if (!property_parent_map.has(p_base_path)) {
		property_parent_map[p_base_path] = HashMap<StringName, StringName>();
	}

	if (node->get_input_count() && !input_activity_map.has(p_base_path)) {
		Vector<Activity> activity;
		for (int i = 0; i < node->get_input_count(); i++) {
			Activity a;
			a.activity = 0;
			a.last_pass = 0;
			activity.push_back(a);
		}
		input_activity_map[p_base_path] = activity;
		input_activity_map_get[String(p_base_path).substr(0, String(p_base_path).length() - 1)] = &input_activity_map[p_base_path];
	}

	List<PropertyInfo> plist;
	node->get_parameter_list(&plist);
	for (PropertyInfo &pinfo : plist) {
		StringName key = pinfo.name;

		if (!property_map.has(p_base_path + key)) {
			property_map[p_base_path + key] = node->get_parameter_default_value(key);
		}

		property_parent_map[p_base_path][key] = p_base_path + key;

		pinfo.name = p_base_path + key;
		properties.push_back(pinfo);
	}

	List<AnimationNode::ChildNode> children;
	node->get_child_nodes(&children);

	for (const AnimationNode::ChildNode &E : children) {
		_update_properties_for_node(p_base_path + E.name + "/", E.node);
	}
}

void AnimationTree::_update_properties() {
	if (!properties_dirty) {
		return;
	}

	properties.clear();
	property_parent_map.clear();
	input_activity_map.clear();
	input_activity_map_get.clear();

	if (root.is_valid()) {
		_update_properties_for_node(SceneStringNames::get_singleton()->parameters_base_path, root);
	}

	properties_dirty = false;

	notify_property_list_changed();
}

bool AnimationTree::_set(const StringName &p_name, const Variant &p_value) {
	if (properties_dirty) {
		_update_properties();
	}

	if (property_map.has(p_name)) {
		property_map[p_name] = p_value;
		return true;
	}

	return false;
}

bool AnimationTree::_get(const StringName &p_name, Variant &r_ret) const {
	if (properties_dirty) {
		const_cast<AnimationTree *>(this)->_update_properties();
	}

	if (property_map.has(p_name)) {
		r_ret = property_map[p_name];
		return true;
	}

	return false;
}

void AnimationTree::_get_property_list(List<PropertyInfo> *p_list) const {
	if (properties_dirty) {
		const_cast<AnimationTree *>(this)->_update_properties();
	}

	for (const PropertyInfo &E : properties) {
		p_list->push_back(E);
	}
}

void AnimationTree::rename_parameter(const String &p_base, const String &p_new_base) {
	//rename values first
	for (const PropertyInfo &E : properties) {
		if (E.name.begins_with(p_base)) {
			String new_name = E.name.replace_first(p_base, p_new_base);
			property_map[new_name] = property_map[E.name];
		}
	}

	//update tree second
	properties_dirty = true;
	_update_properties();
}

real_t AnimationTree::get_connection_activity(const StringName &p_path, int p_connection) const {
	if (!input_activity_map_get.has(p_path)) {
		return 0;
	}
	const Vector<Activity> *activity = input_activity_map_get[p_path];

	if (!activity || p_connection < 0 || p_connection >= activity->size()) {
		return 0;
	}

	if ((*activity)[p_connection].last_pass != process_pass) {
		return 0;
	}

	return (*activity)[p_connection].activity;
}

void AnimationTree::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_active", "active"), &AnimationTree::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &AnimationTree::is_active);

	ClassDB::bind_method(D_METHOD("set_tree_root", "root"), &AnimationTree::set_tree_root);
	ClassDB::bind_method(D_METHOD("get_tree_root"), &AnimationTree::get_tree_root);

	ClassDB::bind_method(D_METHOD("set_process_callback", "mode"), &AnimationTree::set_process_callback);
	ClassDB::bind_method(D_METHOD("get_process_callback"), &AnimationTree::get_process_callback);

	ClassDB::bind_method(D_METHOD("set_animation_player", "root"), &AnimationTree::set_animation_player);
	ClassDB::bind_method(D_METHOD("get_animation_player"), &AnimationTree::get_animation_player);

	ClassDB::bind_method(D_METHOD("set_advance_expression_base_node", "node"), &AnimationTree::set_advance_expression_base_node);
	ClassDB::bind_method(D_METHOD("get_advance_expression_base_node"), &AnimationTree::get_advance_expression_base_node);

	ClassDB::bind_method(D_METHOD("set_root_motion_track", "path"), &AnimationTree::set_root_motion_track);
	ClassDB::bind_method(D_METHOD("get_root_motion_track"), &AnimationTree::get_root_motion_track);

	ClassDB::bind_method(D_METHOD("get_root_motion_transform"), &AnimationTree::get_root_motion_transform);

	ClassDB::bind_method(D_METHOD("_update_properties"), &AnimationTree::_update_properties);

	ClassDB::bind_method(D_METHOD("rename_parameter", "old_name", "new_name"), &AnimationTree::rename_parameter);

	ClassDB::bind_method(D_METHOD("advance", "delta"), &AnimationTree::advance);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tree_root", PROPERTY_HINT_RESOURCE_TYPE, "AnimationRootNode"), "set_tree_root", "get_tree_root");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "anim_player", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AnimationPlayer"), "set_animation_player", "get_animation_player");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "advance_expression_base_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node"), "set_advance_expression_base_node", "get_advance_expression_base_node");

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "process_callback", PROPERTY_HINT_ENUM, "Physics,Idle,Manual"), "set_process_callback", "get_process_callback");
	ADD_GROUP("Root Motion", "root_motion_");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_motion_track"), "set_root_motion_track", "get_root_motion_track");

	BIND_ENUM_CONSTANT(ANIMATION_PROCESS_PHYSICS);
	BIND_ENUM_CONSTANT(ANIMATION_PROCESS_IDLE);
	BIND_ENUM_CONSTANT(ANIMATION_PROCESS_MANUAL);
}

AnimationTree::AnimationTree() {
}

AnimationTree::~AnimationTree() {
}
