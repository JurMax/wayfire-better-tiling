#include "tree.hpp"

#include <iostream>
#include <algorithm>

#include <wayfire/util.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/view-transform.hpp>
// #include <wayfire/plugins/crossfade.hpp>
#include "crossfade.hpp"

namespace wf
{
namespace tile
{
void tree_node_t::set_geometry(wf::geometry_t geometry)
{
    this->geometry = geometry;
}

nonstd::observer_ptr<split_node_t> tree_node_t::as_split_node()
{
    return nonstd::make_observer(dynamic_cast<split_node_t*>(this));
}

nonstd::observer_ptr<view_node_t> tree_node_t::as_view_node()
{
    return nonstd::make_observer(dynamic_cast<view_node_t*>(this));
}

int tree_node_t::get_sibling_index()
{
    auto& children = this->parent->children;
    auto it = std::find_if(children.begin(), children.end(),
        [=] (auto& node) { return node.get() == this; });

    return it - children.begin();
}


wf::point_t get_output_local_coordinates(wf::output_t *output, wf::point_t p)
{
    auto vp   = output->workspace->get_current_workspace();
    auto size = output->get_screen_size();
    p.x -= vp.x * size.width;
    p.y -= vp.y * size.height;

    return p;
}

wf::geometry_t get_output_local_coordinates(wf::output_t *output, wf::geometry_t g)
{
    auto new_tl = get_output_local_coordinates(output, wf::point_t{g.x, g.y});
    g.x = new_tl.x;
    g.y = new_tl.y;

    return g;
}

/* ---------------------- split_node_t implementation ----------------------- */
wf::geometry_t split_node_t::get_child_geometry(
    int32_t child_pos, int32_t child_size)
{
    wf::geometry_t child_geometry = this->geometry;
    switch (get_split_direction())
    {
      case SPLIT_HORIZONTAL:
        child_geometry.y += child_pos;
        child_geometry.height = child_size;
        break;

      case SPLIT_VERTICAL:
        child_geometry.x    += child_pos;
        child_geometry.width = child_size;
        break;
    }

    return child_geometry;
}

int32_t split_node_t::calculate_splittable(wf::geometry_t available) const
{
    switch (get_split_direction())
    {
      case SPLIT_HORIZONTAL:
        return available.height;

      case SPLIT_VERTICAL:
        return available.width;
    }

    return -1;
}

int32_t split_node_t::calculate_splittable() const
{
    return calculate_splittable(this->geometry);
}

void split_node_t::recalculate_children(wf::geometry_t available)
{
    if (this->children.empty())
    {
        return;
    }

    if (this->tabbed)
    {
        for (auto& child : this->children)
        {
            child->set_gaps(this->gaps);
            child->set_geometry(this->geometry);
        }

        return;
    }

    double old_child_sum = 0.0;
    for (auto& child : this->children)
    {
        old_child_sum += calculate_splittable(child->geometry);
    }

    int32_t total_splittable = calculate_splittable(available);

    /* Sum of children sizes up to now */
    double up_to_now = 0.0;

    auto progress = [=] (double current)
    {
        return (current / old_child_sum) * total_splittable;
    };

    set_gaps(this->gaps);

    /* For each child, assign its percentage of the whole. */
    for (auto& child : this->children)
    {
        /* Calculate child_start/end every time using the percentage from the
         * beginning. This way we avoid rounding errors causing empty spaces */
        int32_t child_start = progress(up_to_now);
        up_to_now += calculate_splittable(child->geometry);
        int32_t child_end = progress(up_to_now);

        /* Set new size */
        int32_t child_size = child_end - child_start;
        child->set_geometry(get_child_geometry(child_start, child_size));
    }
}

void split_node_t::add_child(std::unique_ptr<tree_node_t> child, int index, bool recalculate_size)
{
    if (recalculate_size)
    {
        /*
        * Strategy:
        * Calculate the size of the new child relative to the old children, so
        * that proportions are right. After that, rescale all nodes.
        */
        int num_children = this->children.size();

        /* Calculate where the new child should be, in current proportions */
        int size_new_child;
        if (num_children > 0)
        {
            size_new_child =
                (calculate_splittable() + num_children - 1) / num_children;
        } else
        {
            size_new_child = calculate_splittable();
        }

        if ((index == -1) || (index > num_children))
        {
            index = num_children;
        }

        // Set size of the child to make sure it gets properly recalculated later
        child->geometry = get_child_geometry(0, size_new_child);
    }

    /* Add child to the list */
    child->parent = {this};
    this->children.emplace(this->children.begin() + index, std::move(child));
    this->focused_idx = index;

    set_gaps(this->gaps);

    /* Recalculate geometry */
    recalculate_children(geometry);
}

std::unique_ptr<tree_node_t> split_node_t::remove_child(
    nonstd::observer_ptr<tree_node_t> child,
    bool recalculate_geometry)
{
    /* Remove child */
    std::unique_ptr<tree_node_t> result;
    auto it = this->children.begin();

    while (it != this->children.end())
    {
        if (it->get() == child.get())
        {
            result = std::move(*it);
            it     = this->children.erase(it);
        } else
        {
            ++it;
        }
    }

    /* Remaining children have the full geometry */
    if (recalculate_geometry)
        recalculate_children(this->geometry);

    result->parent = nullptr;

    return result;
}

std::unique_ptr<tree_node_t> split_node_t::replace_child(
    nonstd::observer_ptr<tree_node_t> child,
    std::unique_ptr<tree_node_t> new_child)
{
    int idx = child->get_sibling_index();
    std::unique_ptr<tree_node_t> result = std::move(this->children[idx]);
    child->parent = nullptr;
    new_child->set_geometry(child->geometry);
    new_child->parent = {this};
    this->children[idx] = std::move(new_child);
    set_gaps(this->gaps);
    return result;
}

void split_node_t::set_geometry(wf::geometry_t geometry)
{
    tree_node_t::set_geometry(geometry);
    recalculate_children(geometry);
}

void split_node_t::set_gaps(const gap_size_t& gaps)
{
    this->gaps = gaps;
    for (const auto& child : this->children)
    {
        gap_size_t child_gaps = gaps;

        /* See which edges are modified by this split */
        int32_t *first_edge, *second_edge;
        switch (this->split_direction)
        {
          case SPLIT_HORIZONTAL:
            first_edge  = &child_gaps.top;
            second_edge = &child_gaps.bottom;
            break;

          case SPLIT_VERTICAL:
            first_edge  = &child_gaps.left;
            second_edge = &child_gaps.right;
            break;

          default:
            assert(false);
        }

        /* Override internal edges */
        if (child != this->children.front())
        {
            *first_edge = gaps.internal;
        }

        if (child != this->children.back())
        {
            *second_edge = gaps.internal;
        }

        child->set_gaps(child_gaps);
    }
}

split_direction_t split_node_t::get_split_direction() const
{
    return this->split_direction;
}

void split_node_t::set_split_direction(split_direction_t direction)
{
    if (this->split_direction != direction)
    {
        this->split_direction = direction;
        recalculate_children(this->geometry);
        // TODO: keep relative child propertions.
    }
}

bool split_node_t::is_tabbed() const
{
    return this->tabbed;
}

void split_node_t::set_tabbed(bool tabbed)
{
    if (this->tabbed != tabbed)
    {
        this->tabbed = tabbed;
        recalculate_children(this->geometry);
    }
}

static void bring_to_front(wf::output_t* output, nonstd::observer_ptr<tree_node_t> node)
{
    if (auto split = node->as_split_node())
    {
        for (const auto& child : split->children)
        {
            bring_to_front(output, child);
        }
    }
    else if (auto view = node->as_view_node())
    {
        output->workspace->bring_to_front(view->view);
    }
}

void split_node_t::focus(wf::output_t* output, int idx)
{
    // Update the focused node.
    if (idx != -1)
    {
        this->focused_idx = idx;
    }

    if (this->focused_idx >= (int)this->children.size())
    {
        this->focused_idx = (int)this->children.size();
    }

    /* Bring the view to the front. */
    nonstd::observer_ptr<tree_node_t> child = this->children[this->focused_idx];
    bring_to_front(output, child);

    if (auto split_child = child->as_split_node())
    {
        split_child->focus(output);
    }
    else if (auto view_child = child->as_view_node())
    {
        bool was_fullscreen = output->get_active_view()->fullscreen;

        /* This will lower the fullscreen status of the view */
        output->focus_view(view_child->view, true);

        if (was_fullscreen)//TODO && keep_fullscreen_on_adjacent)
        {
            view_child->view->fullscreen_request(output, true);
        }
    }
}

split_node_t::split_node_t(split_direction_t dir)
{
    this->split_direction = dir;
    this->tabbed = false;
    this->focused_idx = 0;
    this->geometry = {0, 0, 0, 0};
}

/* -------------------- view_node_t implementation -------------------------- */
struct view_node_custom_data_t : public custom_data_t
{
    nonstd::observer_ptr<view_node_t> ptr;
    view_node_custom_data_t(view_node_t *node)
    {
        ptr = nonstd::make_observer(node);
    }
};

/**
 * A simple transformer to scale and translate the view in such a way that
 * its displayed wm geometry region is a specified box on the screen
 */
static const std::string scale_transformer_name =
    "better-tiling-scale-transformer";
struct view_node_t::scale_transformer_t : public wf::view_2D
{
    wf::geometry_t box;

    scale_transformer_t(wayfire_view view, wf::geometry_t box) :
        wf::view_2D(view)
    {
        set_box(box);
    }

    void set_box(wf::geometry_t box)
    {
        assert(box.width > 0 && box.height > 0);

        this->view->damage();

        auto current = this->view->get_wm_geometry();
        if ((current.width <= 0) || (current.height <= 0))
        {
            /* view possibly unmapped?? */
            return;
        }

        double scale_horiz = 1.0 * box.width / current.width;
        double scale_vert  = 1.0 * box.height / current.height;

        /* Position of top-left corner after scaling */
        double scaled_x = current.x + (current.width / 2.0 * (1 - scale_horiz));
        double scaled_y = current.y + (current.height / 2.0 * (1 - scale_vert));

        this->scale_x = scale_horiz;
        this->scale_y = scale_vert;
        this->translation_x = box.x - scaled_x;
        this->translation_y = box.y - scaled_y;
    }
};

/**
 * A class for animating the view, emits a signal when the animation is over.
 */
class tile_view_animation_t : public wf::grid::grid_animation_t
{
  public:
    using wf::grid::grid_animation_t::grid_animation_t;

    ~tile_view_animation_t()
    {
        // The grid animation does this too, however, we want to remove the
        // transformer so that we can enforce the correct geometry from the
        // start.
        view->pop_transformer("grid-crossfade");
        view->emit_signal("better-tiling-adjust-transformer", nullptr);
    }

    tile_view_animation_t(const tile_view_animation_t &) = delete;
    tile_view_animation_t(tile_view_animation_t &&) = delete;
    tile_view_animation_t& operator =(const tile_view_animation_t&) = delete;
    tile_view_animation_t& operator =(tile_view_animation_t&&) = delete;
};

view_node_t::view_node_t(wayfire_view view)
{
    this->view = view;
    view->store_data(std::make_unique<view_node_custom_data_t>(this));

    this->on_geometry_changed.set_callback([=] (wf::signal_data_t*)
    {
        update_transformer();
    });
    this->on_decoration_changed.set_callback([=] (wf::signal_data_t*)
    {
        set_geometry(geometry);
    });
    view->connect_signal("geometry-changed", &on_geometry_changed);
    view->connect_signal("decoration-changed", &on_decoration_changed);
    view->connect_signal("better-tiling-adjust-transformer", &on_geometry_changed);
}

view_node_t::~view_node_t()
{
    view->pop_transformer(scale_transformer_name);
    view->erase_data<view_node_custom_data_t>();
}

void view_node_t::set_gaps(const gap_size_t& size)
{
    if ((this->gaps.top != size.top) ||
        (this->gaps.bottom != size.bottom) ||
        (this->gaps.left != size.left) ||
        (this->gaps.right != size.right))
    {
        this->gaps = size;
    }
}

wf::geometry_t view_node_t::calculate_target_geometry()
{
    /* Calculate view geometry in coordinates local to the active workspace,
     * because tree coordinates are kept in workspace-agnostic coordinates. */
    auto output = view->get_output();
    auto local_geometry = get_output_local_coordinates(
        view->get_output(), geometry);

    local_geometry.x     += gaps.left;
    local_geometry.y     += gaps.top;
    local_geometry.width -= gaps.left + gaps.right;
    local_geometry.height -= gaps.top + gaps.bottom;

    auto size = output->get_screen_size();
    /* If view is maximized, we want to use the full available geometry */
    if (view->fullscreen)
    {
        auto vp = output->workspace->get_current_workspace();

        int view_vp_x = std::floor(1.0 * geometry.x / size.width);
        int view_vp_y = std::floor(1.0 * geometry.y / size.height);

        local_geometry = {
            (view_vp_x - vp.x) * size.width,
            (view_vp_y - vp.y) * size.height,
            size.width,
            size.height,
        };
    }

    if (view->sticky)
    {
        local_geometry.x =
            (local_geometry.x % size.width + size.width) % size.width;
        local_geometry.y =
            (local_geometry.y % size.height + size.height) % size.height;
    }

    return local_geometry;
}

bool view_node_t::needs_crossfade()
{
    if (animation_duration == 0)
    {
        return false;
    }

    if (view->has_data<wf::grid::grid_animation_t>())
    {
        return true;
    }

    if (view->get_output()->is_plugin_active("better-tiling"))
    {
        // Disable animations while controllers are active
        return false;
    }

    return true;
}

static nonstd::observer_ptr<wf::grid::grid_animation_t> ensure_animation(
    wayfire_view view, wf::option_sptr_t<int> duration)
{
    if (!view->has_data<wf::grid::grid_animation_t>())
    {
        const auto type = wf::grid::grid_animation_t::CROSSFADE;
        view->store_data<wf::grid::grid_animation_t>(
            std::make_unique<tile_view_animation_t>(view, type, duration));
    }

    return view->get_data<wf::grid::grid_animation_t>();
}

void view_node_t::set_geometry(wf::geometry_t geometry)
{
    tree_node_t::set_geometry(geometry);

    if (!view->is_mapped())
    {
        return;
    }

    view->set_tiled(TILED_EDGES_ALL);

    auto target = calculate_target_geometry();
    if (this->needs_crossfade() && (target != view->get_wm_geometry()))
    {
        if (view->get_transformer(scale_transformer_name))
        {
            view->pop_transformer(scale_transformer_name);
        }
        ensure_animation(view, animation_duration)
            ->adjust_target_geometry(target, -1);
        if (view->get_transformer(scale_transformer_name))
        {
            view->pop_transformer(scale_transformer_name);
        }
    }
    else
    {
        view->set_geometry(target);
    }
}

void view_node_t::update_transformer()
{
    auto target_geometry = calculate_target_geometry();
    if ((target_geometry.width <= 0) || (target_geometry.height <= 0))
    {
        return;
    }

    if (view->has_data<wf::grid::grid_animation_t>())
    {
        // Still animating
        return;
    }

    auto wm = view->get_wm_geometry();
    auto transformer = static_cast<scale_transformer_t*>(
        view->get_transformer(scale_transformer_name).get());

    if (wm != target_geometry)
    {
        if (!transformer)
        {
            auto tr = std::make_unique<scale_transformer_t>(view, target_geometry);
            transformer = tr.get();
            view->add_transformer(std::move(tr), scale_transformer_name);
        } else
        {
            transformer->set_box(target_geometry);
        }
    } else
    {
        if (transformer)
        {
            view->pop_transformer(scale_transformer_name);
        }
    }
}

nonstd::observer_ptr<view_node_t> view_node_t::get_node(wayfire_view view)
{
    if (!view || !view->has_data<view_node_custom_data_t>())
    {
        return nullptr;
    }

    return view->get_data<view_node_custom_data_t>()->ptr;
}

/* ----------------- Generic tree operations implementation ----------------- */
void flatten_tree(std::unique_ptr<tree_node_t>& root)
{
    /* Cannot flatten a view node */
    if (root->as_view_node())
    {
        return;
    }

    /* No flattening required on this level */
    if (root->children.size() >= 1)
    {
        for (auto& child : root->children)
        {
            flatten_tree(child);
        }

        return;
    }

    /* Only the real root of the tree can have no children */
    assert(!root->parent || root->children.size());

    if (root->children.empty())
    {
        return;
    }

    nonstd::observer_ptr<tree_node_t> child_ptr = {root->children.front()};

    /* A single view child => cannot make it root */
    if (child_ptr->as_view_node())
    {
        if (!root->parent)
        {
            return;
        }
    }

    /* Rewire the tree, skipping the current root */
    auto child = root->as_split_node()->remove_child(child_ptr);

    child->parent = root->parent;
    root = std::move(child); // overwrite root with the child
}

nonstd::observer_ptr<split_node_t> get_root(
    nonstd::observer_ptr<tree_node_t> node)
{
    if (!node->parent)
    {
        return {dynamic_cast<split_node_t*>(node.get())};
    }

    return get_root(node->parent);
}
}
}
