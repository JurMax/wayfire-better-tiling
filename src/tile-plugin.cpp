#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

#include "tree-controller.hpp"

#include <iostream>

namespace wf
{
class tile_workspace_implementation_t : public wf::workspace_implementation_t
{
  public:
    bool view_movable(wayfire_view view) override
    {
        return wf::tile::view_node_t::get_node(view) == nullptr;
    }

    bool view_resizable(wayfire_view view) override
    {
        return wf::tile::view_node_t::get_node(view) == nullptr;
    }
};

/**
 * When a view is moved from one output to the other, we want to keep its tiled
 * status. To achieve this, we do the following:
 *
 * 1. In view-pre-moved-to-output handler, we set view_auto_tile_t custom data.
 * 2. In detach handler, we just remove the view as usual.
 * 3. We now know we will receive attach as next event.
 *    Check for view_auto_tile_t, and tile the view again.
 */
class view_auto_tile_t : public wf::custom_data_t
{};

class tile_plugin_t : public wf::plugin_interface_t
{
  private:
    /**
     * Initialize options from configuration file.
     */
    wf::view_matcher_t tile_by_default{"better-tiling/tile_by_default"};
    // TODO: add blacklist for windows that should default to floating.

    wf::option_wrapper_t<bool> keep_fullscreen_on_adjacent{
        "better-tiling/keep_fullscreen_on_adjacent"};
    wf::option_wrapper_t<wf::buttonbinding_t>
        button_move{"better-tiling/button_move"},
        button_resize{"better-tiling/button_resize"};
    wf::option_wrapper_t<wf::keybinding_t> key_toggle_tile{"better-tiling/key_toggle"};

    wf::option_wrapper_t<wf::keybinding_t>
        key_toggle_split_direction{"better-tiling/key_toggle_split_direction"},
        key_split_horizontal{"better-tiling/key_split_horizontal"},
        key_split_vertical{"better-tiling/key_split_vertical"};

    wf::option_wrapper_t<wf::keybinding_t>
        key_toggle_tabbed{"better-tiling/key_toggle_tabbed"};

    wf::option_wrapper_t<wf::keybinding_t>
        key_focus_left{"better-tiling/key_focus_left"},
        key_focus_right{"better-tiling/key_focus_right"},
        key_focus_above{"better-tiling/key_focus_above"},
        key_focus_below{"better-tiling/key_focus_below"};

    wf::option_wrapper_t<wf::keybinding_t>
        key_move_left{"better-tiling/key_move_left"},
        key_move_right{"better-tiling/key_move_right"},
        key_move_above{"better-tiling/key_move_above"},
        key_move_below{"better-tiling/key_move_below"};

    wf::option_wrapper_t<int> inner_gaps{"better-tiling/inner_gap_size"};
    wf::option_wrapper_t<int> outer_horiz_gaps{"better-tiling/outer_horiz_gap_size"};
    wf::option_wrapper_t<int> outer_vert_gaps{"better-tiling/outer_vert_gap_size"};

    /**
     * Initialize other variables and defaults.
     */
    std::vector<std::vector<std::unique_ptr<wf::tile::tree_node_t>>> roots;
    std::vector<std::vector<nonstd::observer_ptr<wf::sublayer_t>>> tiled_sublayer;

    const wf::tile::split_direction_t default_split = wf::tile::SPLIT_VERTICAL;
    wf::tile::split_direction_t split_direction = wf::tile::SPLIT_VERTICAL;

    wf::signal_connection_t on_workspace_grid_changed = [=] (auto)
    {
        resize_roots(output->workspace->get_workspace_grid_size());
    };

    void resize_roots(wf::dimensions_t wsize)
    {
        for (size_t i = 0; i < tiled_sublayer.size(); i++)
        {
            for (size_t j = 0; j < tiled_sublayer[i].size(); j++)
            {
                if (!output->workspace->is_workspace_valid({(int)i, (int)j}))
                {
                    output->workspace->destroy_sublayer(tiled_sublayer[i][j]);
                    roots[i][j].reset();
                }
            }
        }

        roots.resize(wsize.width);
        tiled_sublayer.resize(wsize.width);
        for (int i = 0; i < wsize.width; i++)
        {
            roots[i].resize(wsize.height);
            tiled_sublayer[i].resize(wsize.height);
            for (int j = 0; j < wsize.height; j++)
            {
                roots[i][j] =
                    std::make_unique<wf::tile::split_node_t>(default_split);
                tiled_sublayer[i][j] = output->workspace->create_sublayer(
                    wf::LAYER_WORKSPACE, wf::SUBLAYER_FLOATING);
            }
        }

        update_root_size(output->workspace->get_workarea());
    }

    void update_root_size(wf::geometry_t workarea)
    {
        auto output_geometry = output->get_relative_geometry();
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int i = 0; i < wsize.width; i++)
        {
            for (int j = 0; j < wsize.height; j++)
            {
                /* Set size */
                auto vp_geometry = workarea;
                vp_geometry.x += i * output_geometry.width;
                vp_geometry.y += j * output_geometry.height;
                roots[i][j]->set_geometry(vp_geometry);
            }
        }
    }

    std::function<void()> update_gaps = [=] ()
    {
        tile::gap_size_t gaps = {
            .left   = outer_horiz_gaps,
            .right  = outer_horiz_gaps,
            .top    = outer_vert_gaps,
            .bottom = outer_vert_gaps,
            .internal = inner_gaps,
        };

        for (auto& col : roots)
        {
            for (auto& root : col)
            {
                root->set_gaps(gaps);
                root->set_geometry(root->geometry);
            }
        }
    };

    bool can_tile_view(wayfire_view view)
    {
        if (view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            return false;
        }

        if (view->parent)
        {
            return false;
        }

        return true;
    }

    static std::unique_ptr<wf::tile::tile_controller_t> get_default_controller()
    {
        return std::make_unique<wf::tile::tile_controller_t>();
    }

    std::unique_ptr<wf::tile::tile_controller_t> controller =
        get_default_controller();

    /**
     * Translate coordinates from output-local coordinates to the coordinate
     * system of the tiling trees, depending on the current workspace
     */
    wf::point_t get_global_input_coordinates()
    {
        wf::pointf_t local = output->get_cursor_position();

        auto vp   = output->workspace->get_current_workspace();
        auto size = output->get_screen_size();
        local.x += size.width * vp.x;
        local.y += size.height * vp.y;

        return {(int)local.x, (int)local.y};
    }

    /**
     * Return the tree node corresponding to the focused view, or nullptr.
     */
    nonstd::observer_ptr<tile::view_node_t> get_active_node()
    {
        return tile::view_node_t::get_node(output->get_active_view());
    }

    /** Check whether we currently have a fullscreen tiled view */
    bool has_fullscreen_view()
    {
        auto vp = output->workspace->get_current_workspace();

        int count_fullscreen = 0;
        for_each_view(roots[vp.x][vp.y], [&] (wayfire_view view)
        {
            count_fullscreen += view->fullscreen;
        });

        return count_fullscreen > 0;
    }

    /** Check whether the current pointer focus is tiled view */
    bool has_tiled_focus()
    {
        auto focus = wf::get_core().get_cursor_focus_view();

        return focus && tile::view_node_t::get_node(focus);
    }

    template<class Controller>
    bool start_controller()
    {
        /* No action possible in this case */
        if (has_fullscreen_view() || !has_tiled_focus())
        {
            return false;
        }

        if (!output->activate_plugin(grab_interface))
        {
            return false;
        }

        if (grab_interface->grab())
        {
            auto vp = output->workspace->get_current_workspace();
            controller = std::make_unique<Controller>(
                roots[vp.x][vp.y], get_global_input_coordinates());
        } else
        {
            output->deactivate_plugin(grab_interface);
        }

        return true;
    }

    void stop_controller(bool force_stop)
    {
        if (!output->is_plugin_active(grab_interface->name))
        {
            return;
        }

        // Deactivate plugin, so that others can react to the events
        output->deactivate_plugin(grab_interface);
        if (!force_stop)
        {
            controller->input_released();
        }

        controller = get_default_controller();
    }

    void attach_view(wayfire_view view, wf::point_t vp = {-1, -1})
    {
        if (!can_tile_view(view))
        {
            return;
        }

        stop_controller(true);

        nonstd::observer_ptr<wf::tile::split_node_t> parent_split = nullptr;

        if (vp == wf::point_t{-1, -1})
        {
            vp = output->workspace->get_current_workspace();

            /* Try to add this node to the focused node. */
            auto focused_node = get_active_node();
            if (focused_node && focused_node->parent)
            {
                parent_split = focused_node->parent;

                // /* Create a new split if the split direction of the parent
                //    does not agree with the requested split direction. */
                // if (parent_split->get_split_direction() != split_direction)
                // {
                //     /* Create a new split with the size of the focused node. */
                //     auto new_split = std::make_unique<tile::split_node_t>(split_direction);
                //     new_split->set_geometry(focused_node->geometry);

                //     /* Remove the focused view from its parent and add it to the new split. */
                //     int focused_node_index = focused_node->get_sibling_index();
                //     auto focused_view_ptr = parent_split->remove_child(focused_node);
                //     new_split->add_child(std::move(focused_view_ptr));

                //     /* Add the new split to the original focused view. */
                //     nonstd::observer_ptr<wf::tile::split_node_t> new_parent_split = new_split;
                //     parent_split->add_child(std::move(new_split), focused_node_index);
                //     parent_split = new_parent_split;
                // }
            }
        }

        if (parent_split == nullptr)
        {
            parent_split = roots[vp.x][vp.y]->as_split_node();
        }

        // Add this node to the root.
        auto view_node = std::make_unique<wf::tile::view_node_t>(view);
        parent_split->add_child(std::move(view_node));
        output->workspace->add_view_to_sublayer(view, tiled_sublayer[vp.x][vp.y]);
    }

    bool tile_window_by_default(wayfire_view view)
    {
        return tile_by_default.matches(view) && can_tile_view(view);
    }

    signal_connection_t on_view_attached = [=] (signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        if (view->has_data<view_auto_tile_t>() || tile_window_by_default(view))
        {
            attach_view(view);
        }
    };

    signal_connection_t on_view_unmapped = [=] (signal_data_t *data)
    {
        stop_controller(true);
        auto node = wf::tile::view_node_t::get_node(get_signaled_view(data));
        if (node)
        {
            detach_view(node);
        }
    };

    signal_connection_t on_view_pre_moved_to_output = [=] (signal_data_t *data)
    {
        auto ev   = static_cast<wf::view_pre_moved_to_output_signal*>(data);
        auto node = wf::tile::view_node_t::get_node(ev->view);
        if ((ev->new_output == this->output) && node)
        {
            ev->view->store_data(std::make_unique<wf::view_auto_tile_t>());
        }
    };

    /** Remove the given view from its tiling container */
    void detach_view(nonstd::observer_ptr<tile::view_node_t> view,
        bool reinsert = true)
    {
        stop_controller(true);
        auto wview = view->view;

        auto parent = view->parent;
        parent->remove_child(view);

        /* Remove parents if they are now empty. */
        while (parent->children.size() == 0 && parent->parent != nullptr)
        {
            auto parent_parent = parent->parent;
            parent_parent->remove_child(parent);
            parent = parent_parent;
        }

        // Maybe flatten parent.

        if (wview->fullscreen && wview->is_mapped())
        {
            wview->fullscreen_request(nullptr, false);
        }

        /* Remove from special sublayer */
        if (reinsert)
        {
            output->workspace->add_view(wview, wf::LAYER_WORKSPACE);
        }
    }

    signal_connection_t on_view_detached = [=] (signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        auto view_node = wf::tile::view_node_t::get_node(view);

        if (view_node)
        {
            detach_view(view_node, false);
        }
    };

    signal_connection_t on_workarea_changed = [=] (signal_data_t *data)
    {
        update_root_size(output->workspace->get_workarea());
    };

    signal_connection_t on_tile_request = [=] (signal_data_t *data)
    {
        auto ev = static_cast<view_tile_request_signal*>(data);
        if (ev->carried_out || !tile::view_node_t::get_node(ev->view))
        {
            return;
        }

        // we ignore those requests because we manage the tiled state manually
        ev->carried_out = true;
    };

    void set_view_fullscreen(wayfire_view view, bool fullscreen)
    {
        /* Set fullscreen, and trigger resizing of the views */
        view->set_fullscreen(fullscreen);
        update_root_size(output->workspace->get_workarea());
    }

    signal_connection_t on_fullscreen_request = [=] (signal_data_t *data)
    {
        auto ev = static_cast<view_fullscreen_signal*>(data);
        if (ev->carried_out || !tile::view_node_t::get_node(ev->view))
        {
            return;
        }

        ev->carried_out = true;
        set_view_fullscreen(ev->view, ev->state);
    };

    signal_connection_t on_focus_changed = [=] (signal_data_t *data)
    {
        if (auto view = tile::view_node_t::get_node(get_signaled_view(data)))
        {
            nonstd::observer_ptr<tile::tree_node_t> current = view;

            while (current->parent != nullptr)
            {
                int idx = current->get_sibling_index();
                if (idx != current->parent->get_focused_idx())
                {
                    current->parent->set_focused_idx(idx);
                }
                else
                {
                    break;
                }

                current = current->parent;
            }

            //TODO: do fullscreen view better, prob store a pointer to is somewhere.
            // why would you do this?
            //  if (!view->fullscreen) {
            // auto vp = output->workspace->get_current_workspace();
            // for_each_view(roots[vp.x][vp.y], [&] (wayfire_view view)
            // {
            //     if (view->fullscreen)
            //     {
            //         set_view_fullscreen(view, false);
            //     }
            // });
        }
    };

    void change_view_workspace(wayfire_view view, wf::point_t vp = {-1, -1})
    {
        auto existing_node = wf::tile::view_node_t::get_node(view);
        if (existing_node)
        {
            detach_view(existing_node);
            attach_view(view, vp);
        }
    }

    signal_connection_t on_view_change_workspace = [=] (signal_data_t *data)
    {
        auto ev = (view_change_workspace_signal*)(data);
        if (ev->old_workspace_valid)
        {
            change_view_workspace(ev->view, ev->to);
        }
    };

    signal_connection_t on_view_minimized = [=] (signal_data_t *data)
    {
        auto ev = (view_minimize_request_signal*)data;
        auto existing_node = wf::tile::view_node_t::get_node(ev->view);

        if (ev->state && existing_node)
        {
            detach_view(existing_node);
        }
        else if (!ev->state && tile_window_by_default(ev->view))
        {
            attach_view(ev->view);
        }
    };

    wf::key_callback on_toggle_tiled_state = [=] (auto)
    {
        auto view = output->get_active_view();

        if (view) // Maybe his is important but idk: && output->can_activate_plugin(grab_interface)
        {
            auto existing_node = tile::view_node_t::get_node(view);
            if (existing_node)
            {
                detach_view(existing_node);
                view->tile_request(0);
            }
            else
            {
                attach_view(view);
            }

            return true;
        }

        return false;
    };

    wf::key_callback on_toggle_split_direction = [=] (wf::keybinding_t binding)
    {
        if (auto active_node = get_active_node())
        {
            auto split = active_node->parent;
            if (split->is_tabbed())
            {
                split->set_tabbed(false);
            }
            else
            {
                split->set_split_direction(
                    split->get_split_direction() == tile::SPLIT_HORIZONTAL
                        ? tile::SPLIT_VERTICAL: tile::SPLIT_HORIZONTAL);
            }

            return true;
        }

        return false;
    };

    wf::key_callback on_set_split_direction = [=] (wf::keybinding_t binding)
    {
        // Creating splits in the horizontal direction means
        // vertical splits, confusingly.
        split_direction = binding == key_split_horizontal
            ? tile::SPLIT_VERTICAL : tile::SPLIT_HORIZONTAL;

        /* Try to change the split direction of the active view, or create a new split.*/
        if (auto focused_node = get_active_node())
        {
            auto split = focused_node->parent;

            if (split->children.size() == 1)
            {
                /* Update the direction of the split. */
                if (split->get_split_direction() != split_direction)
                {
                    split->set_split_direction(split_direction);
                }
            }
            else
            {
                /* Create a new split with the size of the focused node. */
                auto new_split = std::make_unique<tile::split_node_t>(split_direction);
                nonstd::observer_ptr<wf::tile::split_node_t> new_split_ptr = new_split;
                auto focused_node_ptr = split->replace_child(focused_node, std::move(new_split));
                new_split_ptr->add_child(std::move(focused_node_ptr));
            }
        }

        return true;
    };

    wf::key_callback on_toggle_tabbed = [=] (wf::keybinding_t binding)
    {
        auto focused_node = get_active_node();
        if (focused_node && focused_node->parent)
        {
            auto split = focused_node->parent->as_split_node();
            split->set_tabbed(!split->is_tabbed());
            return true;
        }

        return false;
    };

    bool focus_adjacent(tile::split_direction_t axis, int direction)
    {
        if (auto active_node = get_active_node())
        {
            /* Try to move the focus through the splits. */
            nonstd::observer_ptr<tile::tree_node_t> current = active_node;
            while (current->parent)
            {
                int idx = current->get_sibling_index() + direction;

                if (current->parent->get_split_direction() != axis
                    || idx < 0 || idx >= (int)current->parent->children.size())
                {
                    current = current->parent;
                }
                else
                {
                    current->parent->focus(output, idx);
                    break;
                }
            }

            return true;
        }


            // bool was_fullscreen = view_node->view->fullscreen;
            // if (adjacent)
            // {
            //     /* This will lower the fullscreen status of the view */
            //     output->focus_view(adjacent->view, true);

            //     if (was_fullscreen && keep_fullscreen_on_adjacent)
            //     {
            //         adjacent->view->fullscreen_request(output, true);
            //     }
            // }
            // return true;

        return false;
    }

    wf::key_callback on_focus_adjacent = [=] (wf::keybinding_t binding)
    {
        if (binding == key_focus_left)
        {
            return focus_adjacent(tile::SPLIT_VERTICAL, -1);
        }
        else if (binding == key_focus_right)
        {
            return focus_adjacent(tile::SPLIT_VERTICAL, 1);
        }
        else if (binding == key_focus_above)
        {
            return focus_adjacent(tile::SPLIT_HORIZONTAL, -1);
        }
        else if (binding == key_focus_below)
        {
            return focus_adjacent(tile::SPLIT_HORIZONTAL, 1);
        }

        return false;
    };

    bool move_adjacent(tile::split_direction_t axis, int direction)
    {
        if (auto view_node = get_active_node())
        {
            nonstd::observer_ptr<tile::tree_node_t> current = view_node;
            auto view_parent = view_node->parent->as_split_node();

            /* Try to move the view through the splits. */
            while (current->parent)
            {
                /* Move one up if the parent split is not the right direction. */
                auto parent = current->parent;
                if (parent->get_split_direction() != axis)
                {
                    current = parent;
                    continue;
                }

                int new_idx = current->get_sibling_index();

                if (current == view_node)
                {
                    new_idx += direction;

                    /* The new index is outside of the range of the current
                     * split, so we must move the view outside of its current
                     * split. */
                    if (new_idx < 0 || new_idx >= (int)parent->children.size())
                    {
                        /* Move one split up. */
                        current = parent;
                        continue;
                    }

                    /* Move the view inside of the neighbour split if able.*/
                    if (auto neighbour = parent->children[new_idx]->as_split_node())
                    {
                        // TODO differ based on split direction.
                        auto ptr = view_node->parent->remove_child(view_node);
                        neighbour->add_child(std::move(ptr), direction == 1 ? 0 : -1);
                        break;
                    }
                }
                else
                {
                    new_idx += direction > 0 ? 1 : 0;
                }

                /* Move the view above/below the node */
                auto ptr = view_node->parent->remove_child(view_node, false);
                parent->add_child(std::move(ptr), new_idx, false);
                break;
            }

            if (view_node->parent != view_parent)
            {
                /* Remove any splits that are now empty. */
                while (view_parent->children.size() == 0
                       && view_parent->parent != nullptr)
                {
                    auto view_parent_parent = view_parent->parent;
                    view_parent_parent->remove_child(view_parent);
                    view_parent = view_parent_parent;
                }
            }

            return true;
        }

        // TODO: create new splits in the root.

        return false;
    }

    wf::key_callback on_move_adjacent = [=] (wf::keybinding_t binding)
    {
        if (binding == key_move_left)
        {
            return move_adjacent(tile::SPLIT_VERTICAL, -1);
        }
        else if (binding == key_move_right)
        {
            return move_adjacent(tile::SPLIT_VERTICAL, 1);
        }
        else if (binding == key_move_above)
        {
            return move_adjacent(tile::SPLIT_HORIZONTAL, -1);
        }
        else if (binding == key_move_below)
        {
            return move_adjacent(tile::SPLIT_HORIZONTAL, 1);
        }

        return false;
    };

    wf::button_callback on_move_view = [=] (auto)
    {
        return start_controller<tile::move_view_controller_t>();
    };

    wf::button_callback on_resize_view = [=] (auto)
    {
        return start_controller<tile::resize_view_controller_t>();
    };

    void setup_callbacks()
    {
        output->add_button(button_move, &on_move_view);
        output->add_button(button_resize, &on_resize_view);
        output->add_key(key_toggle_tile, &on_toggle_tiled_state);

        output->add_key(key_toggle_split_direction, &on_toggle_split_direction);
        output->add_key(key_split_horizontal, &on_set_split_direction);
        output->add_key(key_split_vertical, &on_set_split_direction);
        output->add_key(key_toggle_tabbed, &on_toggle_tabbed);

        output->add_key(key_focus_left, &on_focus_adjacent);
        output->add_key(key_focus_right, &on_focus_adjacent);
        output->add_key(key_focus_above, &on_focus_adjacent);
        output->add_key(key_focus_below, &on_focus_adjacent);

        output->add_key(key_move_left, &on_move_adjacent);
        output->add_key(key_move_right, &on_move_adjacent);
        output->add_key(key_move_above, &on_move_adjacent);
        output->add_key(key_move_below, &on_move_adjacent);

        grab_interface->callbacks.pointer.button =
            [=] (uint32_t b, uint32_t state)
        {
            if (state == WLR_BUTTON_RELEASED)
            {
                stop_controller(false);
            }
        };

        grab_interface->callbacks.pointer.motion = [=] (auto, auto)
        {
            controller->input_motion(get_global_input_coordinates());
        };

        inner_gaps.set_callback(update_gaps);
        outer_horiz_gaps.set_callback(update_gaps);
        outer_vert_gaps.set_callback(update_gaps);
        update_gaps();
    }

  public:
    void init() override
    {
        this->grab_interface->name = "better-tiling";
        /* TODO: change how grab interfaces work - plugins should do ifaces on
         * their own, and should be able to have more than one */
        this->grab_interface->capabilities = CAPABILITY_MANAGE_COMPOSITOR;

        resize_roots(output->workspace->get_workspace_grid_size());
        // TODO: check whether this was successful
        output->workspace->set_workspace_implementation(
            std::make_unique<tile_workspace_implementation_t>(), true);

        output->connect_signal("view-unmapped", &on_view_unmapped);
        output->connect_signal("view-layer-attached", &on_view_attached);
        output->connect_signal("view-layer-detached", &on_view_detached);
        output->connect_signal("workarea-changed", &on_workarea_changed);
        output->connect_signal("view-tile-request", &on_tile_request);
        output->connect_signal("view-fullscreen-request",
            &on_fullscreen_request);
        output->connect_signal("view-focused", &on_focus_changed);
        output->connect_signal("view-change-workspace", &on_view_change_workspace);
        output->connect_signal("view-minimize-request", &on_view_minimized);
        output->connect_signal("workspace-grid-changed",
            &on_workspace_grid_changed);
        wf::get_core().connect_signal("view-pre-moved-to-output",
            &on_view_pre_moved_to_output);

        setup_callbacks();
    }

    void fini() override
    {
        output->workspace->set_workspace_implementation(nullptr, true);

        for (auto& row : tiled_sublayer)
        {
            for (auto& sublayer : row)
            {
                output->workspace->destroy_sublayer(sublayer);
            }
        }

        output->rem_binding(&on_move_view);
        output->rem_binding(&on_resize_view);
        output->rem_binding(&on_toggle_tiled_state);
        output->rem_binding(&on_focus_adjacent);
    }
};
}

DECLARE_WAYFIRE_PLUGIN(wf::tile_plugin_t);
