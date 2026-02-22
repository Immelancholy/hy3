#include <any>
#include <cstdint>
#include <expected>
#include <regex>
#include <set>

#include <dlfcn.h>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <ranges>

#include "log.hpp"
#include "Hy3Layout.hpp"
#include "Hy3Node.hpp"
#include "TabGroup.hpp"
#include "globals.hpp"
#include "src/SharedDefs.hpp"
#include "src/desktop/view/WLSurface.hpp"
#include "src/desktop/view/Window.hpp"
#include "src/desktop/rule/Rule.hpp"
#include "src/desktop/types/OverridableVar.hpp"
#include "src/devices/IPointer.hpp"


using namespace Desktop::View;

// Static member definitions
std::list<Hy3Node> Hy3Layout::nodes;
std::list<Hy3TabGroup> Hy3Layout::tab_groups;
std::vector<Hy3Layout*> Hy3Layout::s_instances;
bool Hy3Layout::s_hooksRegistered = false;

static SP<HOOK_CALLBACK_FN> renderHookPtr;
static SP<HOOK_CALLBACK_FN> windowTitleHookPtr;
static SP<HOOK_CALLBACK_FN> urgentHookPtr;
static SP<HOOK_CALLBACK_FN> tickHookPtr;
static SP<HOOK_CALLBACK_FN> mouseButtonPtr;
static SP<HOOK_CALLBACK_FN> activeWindowHookPtr;

Hy3Layout::Hy3Layout() {
	s_instances.push_back(this);
	if (!s_hooksRegistered) {
		registerHooks();
		s_hooksRegistered = true;
	}
}

Hy3Layout::~Hy3Layout() {
	// Remove nodes belonging to this instance's workspace
	// (nodes track their workspace, we clean up those that reference workspaces
	// whose algorithm is being destroyed)
	std::erase(s_instances, this);
}

Hy3Layout* Hy3Layout::getLayoutForWorkspace(const CWorkspace* workspace) {
	if (!workspace || !workspace->m_space) return nullptr;
	auto algo = workspace->m_space->algorithm();
	if (!algo) return nullptr;
	auto& tiledAlgo = algo->tiledAlgo();
	if (!tiledAlgo) return nullptr;
	return dynamic_cast<Hy3Layout*>(tiledAlgo.get());
}

Hy3Layout* Hy3Layout::getActiveLayout() {
	auto monitor = Desktop::focusState()->monitor();
	if (!monitor) return nullptr;
	auto workspace = monitor->m_activeSpecialWorkspace;
	if (!valid(workspace)) workspace = monitor->m_activeWorkspace;
	if (!valid(workspace)) return nullptr;
	return getLayoutForWorkspace(workspace.get());
}

void Hy3Layout::registerHooks() {
	renderHookPtr = HyprlandAPI::registerCallbackDynamic(PHANDLE, "render", &Hy3Layout::renderHook);

	windowTitleHookPtr = HyprlandAPI::registerCallbackDynamic(
	    PHANDLE,
	    "windowTitle",
	    &Hy3Layout::windowGroupUpdateRecursiveHook
	);

	urgentHookPtr =
	    HyprlandAPI::registerCallbackDynamic(PHANDLE, "urgent", &Hy3Layout::windowGroupUrgentHook);

	tickHookPtr = HyprlandAPI::registerCallbackDynamic(PHANDLE, "tick", &Hy3Layout::tickHook);

	mouseButtonPtr =
	    HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseButton", &Hy3Layout::mouseButtonHook);

	activeWindowHookPtr =
	    HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", &Hy3Layout::activeWindowHook);
}

void Hy3Layout::cleanupStatics() {
	// Reset hook pointers (releases the callbacks)
	renderHookPtr.reset();
	windowTitleHookPtr.reset();
	urgentHookPtr.reset();
	tickHookPtr.reset();
	mouseButtonPtr.reset();
	activeWindowHookPtr.reset();

	// Clear shared static data
	nodes.clear();
	tab_groups.clear();
	s_instances.clear();
	s_hooksRegistered = false;
}

PHLWORKSPACE workspace_for_action(bool allow_fullscreen) {
	if (Hy3Layout::getActiveLayout() == nullptr) return nullptr;

	auto workspace = Desktop::focusState()->monitor()->m_activeSpecialWorkspace;
	if (!valid(workspace)) workspace = Desktop::focusState()->monitor()->m_activeWorkspace;

	if (!valid(workspace)) return nullptr;
	if (!allow_fullscreen && workspace->m_hasFullscreenWindow) return nullptr;

	return workspace;
}

std::string operationWorkspaceForName(const std::string& workspace) {
	typedef std::string (*PHYPRSPLIT_GET_WORKSPACE_FN)(const std::string& workspace);

	static auto* hyprsplitTransformer = []() {
		for (auto& p: g_pPluginSystem->getAllPlugins()) {
			if (p->m_name == "hyprsplit") {
				return reinterpret_cast<PHYPRSPLIT_GET_WORKSPACE_FN>(
				    dlsym(p->m_handle, "hyprsplitGetWorkspace")
				);
			}
		}

		return reinterpret_cast<PHYPRSPLIT_GET_WORKSPACE_FN>(0);
	}();

	if (hyprsplitTransformer != 0) return hyprsplitTransformer(workspace);
	return workspace;
}

// Forward declarations for helpers used below
ShiftDirection reverse(ShiftDirection direction);

static Math::eDirection shiftDirectionToMathDirection(ShiftDirection direction) {
	switch (direction) {
	case ShiftDirection::Left: return Math::DIRECTION_LEFT;
	case ShiftDirection::Right: return Math::DIRECTION_RIGHT;
	case ShiftDirection::Up: return Math::DIRECTION_UP;
	case ShiftDirection::Down: return Math::DIRECTION_DOWN;
	default: return Math::DIRECTION_DEFAULT;
	}
}

// ITiledAlgorithm interface implementations

void Hy3Layout::newTarget(SP<Layout::ITarget> target) {
	auto window = target->window();
	if (window) this->onWindowCreatedTiling(window);
}

void Hy3Layout::movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint) {
	auto window = target->window();
	if (window) {
		this->onWindowCreatedTiling(window);
	}
}

void Hy3Layout::removeTarget(SP<Layout::ITarget> target) {
	auto window = target->window();
	if (window) this->onWindowRemovedTiling(window);
}

void Hy3Layout::resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner) {
	auto window = target->window();
	if (!window) return;

	auto* node = this->getNodeFromWindow(window.get());

	if (node != nullptr) {
		node = &node->getExpandActor();

		auto& monitor = window->m_monitor;

		const bool display_left =
		    STICKS(node->position.x, monitor->m_position.x + monitor->m_reservedArea.left());
		const bool display_right = STICKS(
		    node->position.x + node->size.x,
		    monitor->m_position.x + monitor->m_size.x - monitor->m_reservedArea.right()
		);
		const bool display_top =
		    STICKS(node->position.y, monitor->m_position.y + monitor->m_reservedArea.top());
		const bool display_bottom = STICKS(
		    node->position.y + node->size.y,
		    monitor->m_position.y + monitor->m_size.y - monitor->m_reservedArea.bottom()
		);

		Vector2D resize_delta = delta;
		bool node_is_root =
		    (node->data.is_group() && node->parent == nullptr)
		    || (node->data.is_window() && (node->parent == nullptr || node->parent->parent == nullptr));

		if (node_is_root) {
			if (display_left && display_right) resize_delta.x = 0;
			if (display_top && display_bottom) resize_delta.y = 0;
		}

		if (resize_delta.x != 0 || resize_delta.y != 0) {
			ShiftDirection target_edge_x;
			ShiftDirection target_edge_y;

			if (corner == Layout::CORNER_NONE) {
				target_edge_x = display_right ? ShiftDirection::Left : ShiftDirection::Right;
				target_edge_y = display_bottom ? ShiftDirection::Up : ShiftDirection::Down;

				if (target_edge_x == ShiftDirection::Left) resize_delta.x = -resize_delta.x;
				if (target_edge_y == ShiftDirection::Up) resize_delta.y = -resize_delta.y;
			} else {
				target_edge_x = corner == Layout::CORNER_TOPLEFT || corner == Layout::CORNER_BOTTOMLEFT
				                  ? ShiftDirection::Left
				                  : ShiftDirection::Right;
				target_edge_y = corner == Layout::CORNER_TOPLEFT || corner == Layout::CORNER_TOPRIGHT
				                  ? ShiftDirection::Up
				                  : ShiftDirection::Down;
			}

			auto horizontal_neighbor = node->findNeighbor(target_edge_x);
			auto vertical_neighbor = node->findNeighbor(target_edge_y);

			static const auto animate = ConfigValue<Hyprlang::INT>("misc:animate_manual_resizes");

			if (horizontal_neighbor) {
				horizontal_neighbor->resize(reverse(target_edge_x), resize_delta.x, *animate == 0);
			}

			if (vertical_neighbor) {
				vertical_neighbor->resize(reverse(target_edge_y), resize_delta.y, *animate == 0);
			}
		}
	}
}

void Hy3Layout::recalculate() {
	// Get this instance's workspace via the parent algorithm -> space -> workspace chain
	PHLWORKSPACE thisWorkspace;
	auto algo = m_parent.lock();
	if (algo) {
		auto space = algo->space();
		if (space) thisWorkspace = space->workspace();
	}

	// Recalculate only root groups belonging to this workspace (or all if workspace unknown)
	for (auto& node: this->nodes) {
		if (node.parent == nullptr && node.data.is_group() && !node.reparenting) {
			if (thisWorkspace && node.workspace != thisWorkspace) continue;
			auto& monitor = node.workspace->m_monitor;
			if (monitor) {
				node.position = monitor->m_position + Vector2D(monitor->m_reservedArea.left(), monitor->m_reservedArea.top());
				node.size = monitor->m_size - Vector2D(monitor->m_reservedArea.left(), monitor->m_reservedArea.top()) - Vector2D(monitor->m_reservedArea.right(), monitor->m_reservedArea.bottom());
				node.recalcSizePosRecursive();
			}
		}
	}
}

void Hy3Layout::swapTargets(SP<Layout::ITarget> a, SP<Layout::ITarget> b) {
	auto windowA = a ? a->window() : PHLWINDOW{};
	auto windowB = b ? b->window() : PHLWINDOW{};
	if (!windowA || !windowB || windowA == windowB) return;

	auto* nodeA = this->getNodeFromWindow(windowA.get());
	auto* nodeB = this->getNodeFromWindow(windowB.get());
	if (!nodeA || !nodeB) return;

	Hy3Node::swapData(*nodeA, *nodeB);

	// Recalculate from both root groups
	auto* rootA = nodeA;
	while (rootA->parent) rootA = rootA->parent;
	rootA->recalcSizePosRecursive();

	auto* rootB = nodeB;
	while (rootB->parent) rootB = rootB->parent;
	if (rootB != rootA) rootB->recalcSizePosRecursive();
}

void Hy3Layout::moveTargetInDirection(SP<Layout::ITarget> t, Math::eDirection dir, bool silent) {
	auto window = t->window();
	if (!window) return;

	auto* node = this->getNodeFromWindow(window.get());
	if (node == nullptr) return;

	ShiftDirection shift;
	switch (dir) {
	case Math::DIRECTION_LEFT: shift = ShiftDirection::Left; break;
	case Math::DIRECTION_RIGHT: shift = ShiftDirection::Right; break;
	case Math::DIRECTION_UP: shift = ShiftDirection::Up; break;
	case Math::DIRECTION_DOWN: shift = ShiftDirection::Down; break;
	default: return;
	}

	this->shiftNode(*node, shift, false, false);
}

SP<Layout::ITarget> Hy3Layout::getNextCandidate(SP<Layout::ITarget> old) {
	auto window = old ? old->window() : PHLWINDOW();

	if (window) {
		if (window->m_workspace->m_hasFullscreenWindow) {
			return nullptr; // let the layout manager handle fullscreen
		}

		auto* node = this->getNodeFromWindow(window.get());
		if (node) {
			// Try finding a tiled candidate
			auto* focused = this->getWorkspaceFocusedNode(window->m_workspace.get(), true);
			if (focused && focused->data.is_window()) {
				auto candidateWindow = focused->data.as_window();
				if (candidateWindow != window) {
					// Return the ITarget for this window from the space
					auto space = old->space();
					if (space) {
						for (auto& target : space->targets()) {
							if (!target.expired() && target.lock()->window() == candidateWindow) {
								return target.lock();
							}
						}
					}
				}
			}
		}
	}

	return nullptr;
}

std::expected<void, std::string> Hy3Layout::layoutMsg(const std::string_view& sv) {
	std::string content(sv);
	if (content == "togglesplit") {
		auto window = Desktop::focusState()->window();
		if (window) {
			auto* node = this->getNodeFromWindow(window.get());
			if (node != nullptr && node->parent != nullptr) {
				auto& layout = node->parent->data.as_group().layout;

				switch (layout) {
				case Hy3GroupLayout::SplitH:
					layout = Hy3GroupLayout::SplitV;
					node->parent->recalcSizePosRecursive();
					break;
				case Hy3GroupLayout::SplitV:
					layout = Hy3GroupLayout::SplitH;
					node->parent->recalcSizePosRecursive();
					break;
				case Hy3GroupLayout::Tabbed: break;
				}
			}
		}
	}

	return {};
}

std::optional<Vector2D> Hy3Layout::predictSizeForNewTarget() {
	return std::nullopt;
}

// Original methods adapted

void Hy3Layout::onWindowCreatedTiling(PHLWINDOW window) {
	if (!window) return;

	hy3_log(
	    LOG,
	    "onWindowCreatedTiling called with window {:x} (floating: {}, monitor: {}, workspace: {})",
	    (uintptr_t) window.get(),
	    window->m_isFloating,
	    window->monitorID(),
	    window->m_workspace->m_id
	);

	if (window->m_isFloating) return;

	auto* existing = this->getNodeFromWindow(window.get());
	if (existing != nullptr) {
		hy3_log(
		    ERR,
		    "onWindowCreatedTiling called with a window ({:x}) that is already tiled (node: {:x})",
		    (uintptr_t) window.get(),
		    (uintptr_t) existing
		);
		return;
	}

	this->nodes.push_back({
	    .parent = nullptr,
	    .data = window,
	    .workspace = window->m_workspace,
	    .layout = this,
	});

	this->insertNode(this->nodes.back());
	window->m_workspace->updateWindows();
}

void Hy3Layout::insertNode(Hy3Node& node) {
	if (node.parent != nullptr) {
		hy3_log(
		    ERR,
		    "insertNode called for node {:x} which already has a parent ({:x})",
		    (uintptr_t) &node,
		    (uintptr_t) node.parent
		);
		return;
	}

	if (!valid(node.workspace)) {
		hy3_log(
		    ERR,
		    "insertNode called for node {:x} with invalid workspace id {}",
		    (uintptr_t) &node,
		    node.workspace->m_id
		);
		return;
	}

	node.reparenting = true;
	node.size_ratio = 1.0;

	auto& monitor = node.workspace->m_monitor;

	Hy3Node* opening_into;
	Hy3Node* opening_after = nullptr;

	auto* root = this->getWorkspaceRootGroup(node.workspace.get());

	if (root != nullptr) {
		opening_after = root->getFocusedNode();
		if (opening_after) opening_after = &opening_after->getPlacementActor();

		// opening_after->parent cannot be nullptr
		if (opening_after == root) {
			opening_after =
			    opening_after->intoGroup(Hy3GroupLayout::SplitH, GroupEphemeralityOption::Standard);
		}
	}

	if (opening_after == nullptr) {
		auto last_window = Desktop::focusState()->window();
		if (last_window != nullptr && last_window->m_workspace == node.workspace
		    && !last_window->m_isFloating
		    && (node.data.is_window() || last_window != node.data.as_window())
		    && last_window->m_isMapped)
		{
			opening_after = this->getNodeFromWindow(last_window.get());
		} else {
			auto mouse_window = g_pCompositor->vectorToWindowUnified(
			    g_pInputManager->getMouseCoordsInternal(),
			    RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS
			);

			if (mouse_window != nullptr && mouse_window->m_workspace == node.workspace) {
				opening_after = this->getNodeFromWindow(mouse_window.get());
			}
		}

		if (opening_after) opening_after = &opening_after->getPlacementActor();
	}

	if (opening_after != nullptr
	    && ((node.data.is_group()
	         && (opening_after == &node || node.data.as_group().hasChild(opening_after)))
	        || opening_after->reparenting))
	{
		opening_after = nullptr;
	}

	if (opening_after != nullptr) {
		opening_into = opening_after->parent;
	} else {
		if ((opening_into = this->getWorkspaceRootGroup(node.workspace.get())) == nullptr) {
			static const auto tab_first_window =
			    ConfigValue<Hyprlang::INT>("plugin:hy3:tab_first_window");

			auto width =
			    monitor->m_size.x - monitor->m_reservedArea.right() - monitor->m_reservedArea.left();
			auto height =
			    monitor->m_size.y - monitor->m_reservedArea.bottom() - monitor->m_reservedArea.top();

			this->nodes.push_back({
			    .data = height > width ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
			    .position = monitor->m_position + Vector2D(monitor->m_reservedArea.left(), monitor->m_reservedArea.top()),
			    .size = monitor->m_size - Vector2D(monitor->m_reservedArea.left(), monitor->m_reservedArea.top()) - Vector2D(monitor->m_reservedArea.right(), monitor->m_reservedArea.bottom()),
			    .workspace = node.workspace,
			    .layout = this,
			});

			if (*tab_first_window) {
				auto& parent = this->nodes.back();

				this->nodes.push_back({
				    .parent = &parent,
				    .data = Hy3GroupLayout::Tabbed,
				    .position = parent.position,
				    .size = parent.size,
				    .workspace = node.workspace,
				    .layout = this,
				});

				parent.data.as_group().children.push_back(&this->nodes.back());
			}

			opening_into = &this->nodes.back();
		}
	}

	if (opening_into->data.is_window()) {
		hy3_log(ERR, "opening_into node ({:x}) was not a group node", (uintptr_t) opening_into);
		errorNotif();
		return;
	}

	if (opening_into->workspace != node.workspace) {
		hy3_log(
		    WARN,
		    "opening_into node ({:x}) is on workspace {} which does not match the new window "
		    "(workspace {})",
		    (uintptr_t) opening_into,
		    opening_into->workspace->m_id,
		    node.workspace->m_id
		);
	}

	{
		// clang-format off
		static const auto at_enable = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:enable");
		static const auto at_ephemeral = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:ephemeral_groups");
		static const auto at_trigger_width = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:trigger_width");
		static const auto at_trigger_height = ConfigValue<Hyprlang::INT>("plugin:hy3:autotile:trigger_height");
		// clang-format on

		this->updateAutotileWorkspaces();

		auto& target_group = opening_into->data.as_group();
		if (*at_enable && opening_after != nullptr && target_group.children.size() > 1
		    && target_group.layout != Hy3GroupLayout::Tabbed
		    && this->shouldAutotileWorkspace(opening_into->workspace.get()))
		{
			auto is_horizontal = target_group.layout == Hy3GroupLayout::SplitH;
			auto trigger = is_horizontal ? *at_trigger_width : *at_trigger_height;
			auto target_size = is_horizontal ? opening_into->size.x : opening_into->size.y;
			auto size_after_addition = target_size / (target_group.children.size() + 1);

			if (trigger >= 0 && (trigger == 0 || size_after_addition < trigger)) {
				auto opening_after1 = opening_after->intoGroup(
				    is_horizontal ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
				    *at_ephemeral ? GroupEphemeralityOption::Ephemeral : GroupEphemeralityOption::Standard
				);
				opening_into = opening_after;
				opening_after = opening_after1;
			}
		}
	}

	node.parent = opening_into;
	node.reparenting = false;

	if (opening_after == nullptr) {
		opening_into->data.as_group().children.push_back(&node);
	} else {
		auto& children = opening_into->data.as_group().children;
		auto iter = std::find(children.begin(), children.end(), opening_after);
		auto iter2 = std::next(iter);
		children.insert(iter2, &node);
	}

	hy3_log(
	    LOG,
	    "tiled node {:x} inserted after node {:x} in node {:x}",
	    (uintptr_t) &node,
	    (uintptr_t) opening_after,
	    (uintptr_t) opening_into
	);

	node.markFocused();
	opening_into->recalcSizePosRecursive();
}

void Hy3Layout::onWindowRemovedTiling(PHLWINDOW window) {
	static const auto node_collapse_policy =
	    ConfigValue<Hyprlang::INT>("plugin:hy3:node_collapse_policy");

	auto* node = this->getNodeFromWindow(window.get());

	if (node == nullptr) return;

	hy3_log(
	    LOG,
	    "removing window ({:x} as node {:x}) from node {:x}",
	    (uintptr_t) window.get(),
	    (uintptr_t) node,
	    (uintptr_t) node->parent
	);

	window->m_ruleApplicator->resetProps(Desktop::Rule::RULE_PROP_ALL, Desktop::Types::PRIORITY_LAYOUT);

	if (window->isFullscreen()) {
		g_pCompositor->setWindowFullscreenInternal(window, FSMODE_NONE);
	}

	Hy3Node* expand_actor = nullptr;
	auto* parent = node->removeFromParentRecursive(&expand_actor);
	this->nodes.remove(*node);
	if (expand_actor != nullptr) expand_actor->recalcSizePosRecursive();

	if (parent != nullptr) {
		auto& group = parent->data.as_group();
		parent->recalcSizePosRecursive();

		// returns if a given node is a group that can be collapsed given the current config
		auto node_is_collapsible = [](Hy3Node* node) {
			if (node->data.is_window()) return false;
			if (*node_collapse_policy == 0) return true;
			else if (*node_collapse_policy == 1) return false;
			return node->parent->data.as_group().layout != Hy3GroupLayout::Tabbed;
		};

		if (group.children.size() == 1
		    && (group.ephemeral || node_is_collapsible(group.children.front())))
		{
			auto* target_parent = parent;
			while (target_parent != nullptr && Hy3Node::swallowGroups(target_parent)) {
				target_parent = target_parent->parent;
			}

			if (target_parent != parent && target_parent != nullptr)
				target_parent->recalcSizePosRecursive();
		}
	}

	window->m_workspace->updateWindows();
}

void Hy3Layout::onWindowFocusChange(PHLWINDOW window) {
	auto* node = this->getNodeFromWindow(window.get());
	if (node == nullptr) return;

	hy3_log(
	    TRACE,
	    "changing window focus to window {:x} as node {:x}",
	    (uintptr_t) window.get(),
	    (uintptr_t) node
	);

	node->markFocused();
	while (node->parent != nullptr) node = node->parent;
	node->recalcSizePosRecursive();
}

ShiftDirection reverse(ShiftDirection direction) {
	switch (direction) {
	case ShiftDirection::Left: return ShiftDirection::Right;
	case ShiftDirection::Right: return ShiftDirection::Left;
	case ShiftDirection::Up: return ShiftDirection::Down;
	case ShiftDirection::Down: return ShiftDirection::Up;
	default: return direction;
	}
}

PHLWINDOW Hy3Layout::findTiledWindowCandidate(const CWindow* from) {
	auto* node = this->getWorkspaceFocusedNode(from->m_workspace.get(), true);
	if (node != nullptr && node->data.is_window()) {
		return node->data.as_window();
	}

	return PHLWINDOW();
}

PHLWINDOW Hy3Layout::findFloatingWindowCandidate(const CWindow* from) {
	// return the first floating window on the same workspace that has not asked not to be focused
	for (auto& w: g_pCompositor->m_windows | std::views::reverse) {
		if (w->m_isMapped && !w->isHidden() && w->m_isFloating && !w->isX11OverrideRedirect()
		    && w->m_workspace == from->m_workspace && !w->m_X11ShouldntFocus
		    && !w->m_ruleApplicator->noFocus().valueOrDefault() && w.get() != from)
		{
			return w;
		}
	}

	return nullptr;
}

void Hy3Layout::makeGroupOnWorkspace(
    const CWorkspace* workspace,
    Hy3GroupLayout layout,
    GroupEphemeralityOption ephemeral,
    bool toggle
) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node) node = &node->getPlacementActor();

	if (node && toggle) {
		auto* parent = node->parent;
		auto& group = parent->data.as_group();

		if (group.children.size() == 1 && group.layout == layout) {
			group.children.clear();
			Hy3Node::swapData(*node, *parent);
			this->nodes.remove(*node); // now the parent

			if (auto* pp = node->parent->parent) {
				pp->updateTabBarRecursive();
				pp->recalcSizePosRecursive();
			}

			return;
		}
	}

	this->makeGroupOn(node, layout, ephemeral);
}

void Hy3Layout::makeOppositeGroupOnWorkspace(
    const CWorkspace* workspace,
    GroupEphemeralityOption ephemeral
) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node) node = &node->getPlacementActor();
	this->makeOppositeGroupOn(node, ephemeral);
}

void Hy3Layout::changeGroupOnWorkspace(const CWorkspace* workspace, Hy3GroupLayout layout) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;
	node = &node->getPlacementActor();

	this->changeGroupOn(*node, layout);
}

void Hy3Layout::untabGroupOnWorkspace(const CWorkspace* workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;
	node = &node->getPlacementActor();

	this->untabGroupOn(*node);
}

void Hy3Layout::toggleTabGroupOnWorkspace(const CWorkspace* workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;
	node = &node->getPlacementActor();

	this->toggleTabGroupOn(*node);
}

void Hy3Layout::changeGroupToOppositeOnWorkspace(const CWorkspace* workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;
	node = &node->getPlacementActor();

	this->changeGroupToOppositeOn(*node);
}

void Hy3Layout::changeGroupEphemeralityOnWorkspace(const CWorkspace* workspace, bool ephemeral) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;
	node = &node->getPlacementActor();

	this->changeGroupEphemeralityOn(*node, ephemeral);
}

void Hy3Layout::makeGroupOn(
    Hy3Node* node,
    Hy3GroupLayout layout,
    GroupEphemeralityOption ephemeral
) {
	if (node == nullptr) return;

	if (node->parent != nullptr) {
		auto& group = node->parent->data.as_group();
		if (group.children.size() == 1) {
			group.setLayout(layout);
			group.setEphemeral(ephemeral);
			node->parent->updateTabBarRecursive();
			node->parent->recalcSizePosRecursive();
			return;
		}
	}

	node->intoGroup(layout, ephemeral);
}

void Hy3Layout::makeOppositeGroupOn(Hy3Node* node, GroupEphemeralityOption ephemeral) {
	if (node == nullptr) return;

	if (node->parent == nullptr) {
		node->intoGroup(Hy3GroupLayout::SplitH, ephemeral);
		return;
	}

	auto& group = node->parent->data.as_group();
	auto layout =
	    group.layout == Hy3GroupLayout::SplitH ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH;

	if (group.children.size() == 1) {
		group.setLayout(layout);
		group.setEphemeral(ephemeral);
		node->parent->recalcSizePosRecursive();
		return;
	}

	node->intoGroup(layout, ephemeral);
}

void Hy3Layout::changeGroupOn(Hy3Node& node, Hy3GroupLayout layout) {
	if (node.parent == nullptr) {
		makeGroupOn(&node, layout, GroupEphemeralityOption::Ephemeral);
		return;
	}

	auto& group = node.parent->data.as_group();
	group.setLayout(layout);
	node.parent->updateTabBarRecursive();
	node.parent->recalcSizePosRecursive();
}

void Hy3Layout::untabGroupOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group();
	if (group.layout != Hy3GroupLayout::Tabbed) return;

	changeGroupOn(node, group.previous_nontab_layout);
}

void Hy3Layout::toggleTabGroupOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group();
	if (group.layout != Hy3GroupLayout::Tabbed) changeGroupOn(node, Hy3GroupLayout::Tabbed);
	else changeGroupOn(node, group.previous_nontab_layout);
}

void Hy3Layout::changeGroupToOppositeOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group();

	if (group.layout == Hy3GroupLayout::Tabbed) {
		group.setLayout(group.previous_nontab_layout);
	} else {
		group.setLayout(
		    group.layout == Hy3GroupLayout::SplitH ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH
		);
	}

	node.parent->recalcSizePosRecursive();
}

void Hy3Layout::changeGroupEphemeralityOn(Hy3Node& node, bool ephemeral) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group();
	group.setEphemeral(
	    ephemeral ? GroupEphemeralityOption::ForceEphemeral : GroupEphemeralityOption::Standard
	);
}

void Hy3Layout::shiftNode(Hy3Node& node, ShiftDirection direction, bool once, bool visible) {
	if (once) {
		auto& n = node.getPlacementActor();
		if (n.parent != nullptr && n.parent->data.as_group().children.size() == 1) {
			if (n.parent->parent == nullptr) {
				n.parent->data.as_group().setLayout(Hy3GroupLayout::SplitH);
				n.parent->recalcSizePosRecursive();
			} else {
				auto* n2 = n.parent;
				Hy3Node::swapData(n, *n2);
				n2->layout->nodes.remove(n);
				n2->updateTabBarRecursive();
				n2->recalcSizePosRecursive();
			}

			return;
		}
	}

	this->shiftOrGetFocus(&node, direction, true, once, visible);
}

void Hy3Layout::shiftWindow(
    const CWorkspace* workspace,
    ShiftDirection direction,
    bool once,
    bool visible
) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->shiftNode(*node, direction, once, visible);
}

void Hy3Layout::shiftFocus(
    const CWorkspace* workspace,
    ShiftDirection direction,
    bool visible,
    bool warp
) {
	auto current_window = Desktop::focusState()->window();

	if (current_window != nullptr) {
		if (current_window->m_workspace->m_hasFullscreenWindow) {
			return;
		}

		if (current_window->m_isFloating) {
			auto next_window =
			    g_pCompositor->getWindowInDirection(current_window, shiftDirectionToMathDirection(direction));

			if (next_window != nullptr) {
				g_pInputManager->unconstrainMouse();
				Desktop::focusState()->fullWindowFocus(next_window, Desktop::FOCUS_REASON_OTHER);
				if (warp) Hy3Layout::warpCursorToBox(next_window->m_position, next_window->m_size);
			}
			return;
		}
	}

	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) {
		focusMonitor(direction);
		return;
	}

	auto* target = this->shiftOrGetFocus(node, direction, false, false, visible);

	if (target != nullptr) {
		if (warp) {
			// don't warp for nodes in the same tab
			warp = node->parent == nullptr || target->parent == nullptr || node->parent != target->parent
			    || node->parent->data.as_group().layout != Hy3GroupLayout::Tabbed;
		}

		target->focus(warp);
		while (target->parent != nullptr) target = target->parent;
		target->recalcSizePosRecursive();
	}
}

Hy3Node* Hy3Layout::focusMonitor(ShiftDirection direction) {
	auto next_monitor = g_pCompositor->getMonitorInDirection(shiftDirectionToMathDirection(direction));

	if (next_monitor) {
		bool found = false;
		Desktop::focusState()->rawMonitorFocus(next_monitor);
		auto next_workspace = next_monitor->m_activeWorkspace;

		if (next_workspace) {
			auto target_window = next_workspace->getLastFocusedWindow();
			if (target_window) {
				found = true;

				// Move the cursor to the window we selected
				auto found_node = getNodeFromWindow(target_window.get());
				if (found_node) {
					found_node->focus(true);
					return found_node;
				}
			}
		}

		if (!found) {
			Hy3Layout::warpCursorWithFocus(next_monitor->m_position + next_monitor->m_size / 2);
		}
	}
	return nullptr;
}

bool Hy3Layout::shiftMonitor(Hy3Node& node, ShiftDirection direction, bool follow) {
	auto next_monitor = g_pCompositor->getMonitorInDirection(shiftDirectionToMathDirection(direction));

	if (next_monitor) {
		Desktop::focusState()->rawMonitorFocus(next_monitor);
		auto next_workspace = next_monitor->m_activeWorkspace;
		if (next_workspace) {
			moveNodeToWorkspace(node.workspace.get(), next_workspace->m_name, follow, false);
			return true;
		}
	}
	return false;
}

void Hy3Layout::toggleFocusLayer(const CWorkspace* workspace, bool warp) {
	auto current_window = Desktop::focusState()->window();
	if (!current_window) return;

	PHLWINDOW target;
	if (current_window->m_isFloating) {
		target = this->findTiledWindowCandidate(current_window.get());
	} else {
		target = this->findFloatingWindowCandidate(current_window.get());
	}

	if (!target) return;

	Desktop::focusState()->fullWindowFocus(target, Desktop::FOCUS_REASON_OTHER);

	if (warp) {
		Hy3Layout::warpCursorWithFocus(target->middle());
	}
}

void Hy3Layout::warpCursor() {
	auto current_window = Desktop::focusState()->window();

	if (current_window != nullptr) {
		if (current_window != nullptr) {
			Hy3Layout::warpCursorWithFocus(current_window->middle(), true);
		}
	} else {
		auto* node =
		    this->getWorkspaceFocusedNode(Desktop::focusState()->monitor()->m_activeWorkspace.get());

		if (node != nullptr) {
			Hy3Layout::warpCursorWithFocus(node->position + node->size / 2);
		}
	}
}

void changeNodeWorkspaceRecursive(Hy3Node& node, PHLWORKSPACE workspace, Hy3Layout* destLayout) {
	node.workspace = workspace;
	if (destLayout) node.layout = destLayout;

	if (node.data.is_window()) {
		auto window = node.data.as_window();
		g_pHyprRenderer->damageWindow(window);
		window->moveToWorkspace(workspace);
		window->m_monitor = workspace->m_monitor;
		window->updateToplevel();
		Desktop::Rule::ruleEngine()->updateAllRules();
	} else {
		for (auto* child: node.data.as_group().children) {
			changeNodeWorkspaceRecursive(*child, workspace, destLayout);
		}
	}
}

void Hy3Layout::moveNodeToWorkspace(
    CWorkspace* origin,
    std::string wsname,
    bool follow,
    bool warp
) {
	auto target = getWorkspaceIDNameFromString(operationWorkspaceForName(wsname));

	if (target.id == WORKSPACE_INVALID) {
		hy3_log(ERR, "moveNodeToWorkspace called with invalid workspace {}", wsname);
		return;
	}

	auto workspace = g_pCompositor->getWorkspaceByID(target.id);

	if (origin == workspace.get()) return;

	auto* node = this->getWorkspaceFocusedNode(origin);
	auto focused_window = Desktop::focusState()->window();
	auto* focused_window_node = this->getNodeFromWindow(focused_window.get());

	auto origin_ws = node != nullptr           ? node->workspace
	               : focused_window != nullptr ? focused_window->m_workspace
	                                           : nullptr;

	if (!valid(origin_ws)) return;

	if (workspace == nullptr) {
		hy3_log(LOG, "creating target workspace {} for node move", target.id);

		workspace = g_pCompositor->createNewWorkspace(target.id, origin_ws->monitorID(), target.name);
	}

	// floating or fullscreen
	if (focused_window != nullptr
	    && (focused_window_node == nullptr || focused_window->isFullscreen()))
	{
		g_pHyprRenderer->damageWindow(focused_window);
		g_pCompositor->moveWindowToWorkspaceSafe(focused_window, workspace);
	} else {
		if (node == nullptr) return;

		hy3_log(
		    LOG,
		    "moving node {:x} from workspace {} to workspace {} (follow: {})",
		    (uintptr_t) node,
		    origin->m_id,
		    workspace->m_id,
		    follow
		);

		Hy3Node* expand_actor = nullptr;
		node->removeFromParentRecursive(&expand_actor);
		if (expand_actor != nullptr) expand_actor->recalcSizePosRecursive();

		// Use the destination workspace's layout instance for insertion
		auto* destLayout = Hy3Layout::getLayoutForWorkspace(workspace.get());
		changeNodeWorkspaceRecursive(*node, workspace, destLayout ? destLayout : this);
		if (destLayout) {
			destLayout->insertNode(*node);
		} else {
			// Fallback: destination workspace may not have hy3 yet; use this instance
			this->insertNode(*node);
		}
		origin->updateWindows();
		workspace->updateWindows();
	}

	if (follow) {
		auto& monitor = workspace->m_monitor;

		if (workspace->m_isSpecialWorkspace) {
			monitor->setSpecialWorkspace(workspace);
		} else if (origin_ws->m_isSpecialWorkspace) {
			origin_ws->m_monitor->setSpecialWorkspace(nullptr);
		}

		monitor->changeWorkspace(workspace);

		node->parent->recalcSizePosRecursive();
		node->focus(warp);
	}
}

void Hy3Layout::changeFocus(const CWorkspace* workspace, FocusShift shift) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	switch (shift) {
	case FocusShift::Bottom: goto bottom;
	case FocusShift::Top:
		while (node->parent != nullptr) {
			node = node->parent;
		}

		node->focus(false);
		return;
	case FocusShift::Raise:
		if (node->parent == nullptr) goto bottom;
		else {
			node->parent->focus(false);
		}
		return;
	case FocusShift::Lower:
		if (node->data.is_group() && node->data.as_group().focused_child != nullptr)
			node->data.as_group().focused_child->focus(false);
		return;
	case FocusShift::Tab:
		// make sure we go up at least one level
		if (node->parent != nullptr) node = node->parent;
		while (node->parent != nullptr) {
			if (node->data.as_group().layout == Hy3GroupLayout::Tabbed) {
				node->focus(false);
				return;
			}

			node = node->parent;
		}
		return;
	case FocusShift::TabNode:
		// make sure we go up at least one level
		if (node->parent != nullptr) node = node->parent;
		while (node->parent != nullptr) {
			if (node->parent->data.as_group().layout == Hy3GroupLayout::Tabbed) {
				node->focus(false);
				return;
			}

			node = node->parent;
		}
		return;
	}

bottom:
	while (node->data.is_group() && node->data.as_group().focused_child != nullptr) {
		node = node->data.as_group().focused_child;
	}

	node->focus(false);
	return;
}

Hy3Node* findTabBarAt(Hy3Node& node, Vector2D pos, Hy3Node** focused_node) {
	// clang-format off
	static const auto p_gaps_in = ConfigValue<Hyprlang::CUSTOMTYPE, CCssGapData>("general:gaps_in");
	static const auto tab_bar_height = ConfigValue<Hyprlang::INT>("plugin:hy3:tabs:height");
	static const auto tab_bar_padding = ConfigValue<Hyprlang::INT>("plugin:hy3:tabs:padding");
	// clang-format on

	auto workspace_rule = g_pConfigManager->getWorkspaceRuleFor(node.workspace);
	auto gaps_in = workspace_rule.gapsIn.value_or(*p_gaps_in);

	auto inset = *tab_bar_height + *tab_bar_padding + gaps_in.m_top;

	if (node.data.is_group()) {
		if (node.hidden) return nullptr;
		// note: tab bar clicks ignore animations
		if (node.position.x > pos.x || node.position.y > pos.y || node.position.x + node.size.x < pos.x
		    || node.position.y + node.size.y < pos.y)
			return nullptr;

		auto& group = node.data.as_group();

		if (group.layout == Hy3GroupLayout::Tabbed && node.data.as_group().tab_bar != nullptr) {
			if (pos.y < node.position.y + node.gap_topleft_offset.y + inset) {
				auto& children = group.children;
				auto& tab_bar = *group.tab_bar;

				auto size = tab_bar.size->value();
				auto x = pos.x - tab_bar.pos->value().x;
				auto child_iter = children.begin();

				for (auto& tab: tab_bar.bar.entries) {
					if (child_iter == children.end()) break;

					if (x > tab.offset->value() * size.x
					    && x < (tab.offset->value() + tab.width->value()) * size.x)
					{
						*focused_node = *child_iter;
						return &node;
					}

					child_iter = std::next(child_iter);
				}
			}

			if (group.focused_child != nullptr) {
				return findTabBarAt(*group.focused_child, pos, focused_node);
			}
		} else {
			for (auto child: group.children) {
				if (findTabBarAt(*child, pos, focused_node)) return child;
			}
		}
	}

	return nullptr;
}

void Hy3Layout::focusTab(
    const CWorkspace* workspace,
    TabFocus target,
    TabFocusMousePriority mouse,
    bool wrap_scroll,
    int index
) {
	auto* node = this->getWorkspaceRootGroup(workspace);
	if (node == nullptr) return;

	Hy3Node* tab_node = nullptr;
	Hy3Node* tab_focused_node;

	if (target == TabFocus::MouseLocation || mouse != TabFocusMousePriority::Ignore) {
		// no surf focused at all
		auto ptrSurfaceResource = g_pSeatManager->m_state.pointerFocus.lock();
		if (!ptrSurfaceResource) return;

		auto ptrSurface = CWLSurface::fromResource(ptrSurfaceResource);
		if (!ptrSurface) return;

		// non window-parented surface focused, cant have a tab
		auto view = ptrSurface->view();
		auto* window = dynamic_cast<CWindow*>(view.get());
		if (!window || window->m_isFloating) return;

		auto mouse_pos = g_pInputManager->getMouseCoordsInternal();
		tab_node = findTabBarAt(*node, mouse_pos, &tab_focused_node);
		if (tab_node != nullptr) goto hastab;

		if (target == TabFocus::MouseLocation || mouse == TabFocusMousePriority::Require) return;
	}

	if (tab_node == nullptr) {
		tab_node = this->getWorkspaceFocusedNode(workspace);
		if (tab_node == nullptr) return;

		while (tab_node != nullptr
		       && (tab_node->data.is_window()
		           || tab_node->data.as_group().layout != Hy3GroupLayout::Tabbed)
		       && tab_node->parent != nullptr)
			tab_node = tab_node->parent;

		if (tab_node == nullptr || tab_node->data.is_window()
		    || tab_node->data.as_group().layout != Hy3GroupLayout::Tabbed)
			return;
	}

hastab:
	if (target != TabFocus::MouseLocation) {
		auto& group = tab_node->data.as_group();
		if (group.focused_child == nullptr || group.children.size() < 2) return;

		auto& children = group.children;
		if (target == TabFocus::Index) {
			int i = 1;

			for (auto* node: children) {
				if (i == index) {
					tab_focused_node = node;
					goto cont;
				}

				i++;
			}

			return;
		cont:;
		} else {
			auto node_iter = std::find(children.begin(), children.end(), group.focused_child);
			if (node_iter == children.end()) return;
			if (target == TabFocus::Left) {
				if (node_iter == children.begin()) {
					if (wrap_scroll) node_iter = std::prev(children.end());
					else return;
				} else node_iter = std::prev(node_iter);

				tab_focused_node = *node_iter;
			} else {
				if (node_iter == std::prev(children.end())) {
					if (wrap_scroll) node_iter = children.begin();
					else return;
				} else node_iter = std::next(node_iter);

				tab_focused_node = *node_iter;
			}
		}
	}

	auto* focus = tab_focused_node;
	while (focus->data.is_group() && !focus->data.as_group().group_focused
	       && focus->data.as_group().focused_child != nullptr)
		focus = focus->data.as_group().focused_child;

	focus->focus(false);
	tab_node->recalcSizePosRecursive();
}

void Hy3Layout::setNodeSwallow(const CWorkspace* workspace, SetSwallowOption option) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr || node->parent == nullptr) return;

	auto* containment = &node->parent->data.as_group().containment;
	switch (option) {
	case SetSwallowOption::NoSwallow: *containment = false; break;
	case SetSwallowOption::Swallow: *containment = true; break;
	case SetSwallowOption::Toggle: *containment = !*containment; break;
	}
}

void Hy3Layout::killFocusedNode(const CWorkspace* workspace) {
	auto last_window = Desktop::focusState()->window();
	if (last_window != nullptr && last_window->m_isFloating) {
		g_pCompositor->closeWindow(last_window);
	} else {
		auto* node = this->getWorkspaceFocusedNode(workspace);
		if (node == nullptr) return;

		std::vector<PHLWINDOW> windows;
		node->appendAllWindows(windows);

		for (auto& window: windows) {
			window->setHidden(false);
			g_pCompositor->closeWindow(window);
		}
	}
}

void Hy3Layout::expand(
    const CWorkspace* workspace,
    ExpandOption option,
    ExpandFullscreenOption fs_option
) {
	auto* node = this->getWorkspaceFocusedNode(workspace, false, true);
	if (node == nullptr) return;
	PHLWINDOW window;

	// const auto monitor = g_pCompositor->getMonitorFromID(workspace->m_iMonitorID);

	switch (option) {
	case ExpandOption::Expand: {
		if (node->parent == nullptr) {
			switch (fs_option) {
			case ExpandFullscreenOption::MaximizeAsFullscreen:
			case ExpandFullscreenOption::MaximizeIntermediate: // goto fullscreen;
			case ExpandFullscreenOption::MaximizeOnly: return;
			}
		}

		if (node->data.is_group() && !node->data.as_group().group_focused)
			node->data.as_group().expand_focused = ExpandFocusType::Stack;

		auto& group = node->parent->data.as_group();
		group.focused_child = node;
		group.expand_focused = ExpandFocusType::Latch;

		node->parent->recalcSizePosRecursive();

		if (node->parent->parent == nullptr) {
			switch (fs_option) {
			case ExpandFullscreenOption::MaximizeAsFullscreen: // goto fullscreen;
			case ExpandFullscreenOption::MaximizeIntermediate:
			case ExpandFullscreenOption::MaximizeOnly: return;
			}
		}
	} break;
	case ExpandOption::Shrink:
		if (node->data.is_group()) {
			auto& group = node->data.as_group();

			group.expand_focused = ExpandFocusType::NotExpanded;
			if (group.focused_child->data.is_group())
				group.focused_child->data.as_group().expand_focused = ExpandFocusType::Latch;

			node->recalcSizePosRecursive();
		}
		break;
	case ExpandOption::Base: {
		if (node->data.is_group()) {
			node->data.as_group().collapseExpansions();
			node->recalcSizePosRecursive();
		}
		break;
	}
	case ExpandOption::Maximize: break;
	case ExpandOption::Fullscreen: break;
	}

	return;
	/*
	fullscreen:
	  if (node->data.is_group()) return;
	  window = node->data.as_window();
	  if (!window->m_bIsFullscreen || window->m_workspace->m_bIsSpecialWorkspace) return;

	  if (workspace->m_bHasFullscreenWindow) return;

	  window->m_bIsFullscreen = true;
	  workspace->m_bHasFullscreenWindow = true;
	  workspace->m_efFullscreenMode = FULLSCREEN_FULL;
	  window->m_realPosition = monitor->vecPosition;
	  window->m_realSize = monitor->m_size;
	  goto fsupdate;
	// unfullscreen:
	// 	if (node->data.type != Hy3NodeType::Window) return;
	// 	window = node->data.as_window;
	// 	window->m_bIsFullscreen = false;
	// 	workspace->m_bHasFullscreenWindow = false;
	// 	goto fsupdate;
	fsupdate:
	  g_pCompositor->updateWindowAnimatedDecorationValues(window);
	  g_pXWaylandManager->setWindowSize(window, window->m_realSize.goal());
	  g_pCompositor->changeWindowZOrder(window, true);
	  this->recalculateMonitor(monitor->ID);*/
}

void Hy3Layout::setTabLock(const CWorkspace* workspace, TabLockMode mode) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr || node->parent == nullptr) return;
	node = node->parent;

	while (node->parent != nullptr
	       && (!node->data.is_group() || node->data.as_group().layout != Hy3GroupLayout::Tabbed))
		node = node->parent;

	if (node == nullptr) return;

	auto& group = node->data.as_group();
	switch (mode) {
	case TabLockMode::Lock: group.locked = true; break;
	case TabLockMode::Unlock: group.locked = false; break;
	case TabLockMode::Toggle: group.locked = !group.locked; break;
	}

	node->updateTabBar();
}

static void equalizeRecursive(Hy3Node* node, bool recursive) {
	node->size_ratio = 1.0f;

	if (recursive && node->data.is_group()) {
		for (auto* child: node->data.as_group().children) {
			equalizeRecursive(child, true);
		}
	}
}

void Hy3Layout::equalize(const CWorkspace* workspace, bool recursive) {
	auto* focused = this->getWorkspaceFocusedNode(workspace);
	if (focused == nullptr) return;

	Hy3Node* target = nullptr;

	if (recursive) {
		target = this->getWorkspaceRootGroup(workspace);
		if (target != nullptr) {
			equalizeRecursive(target, true);
		}
	} else {
		if (focused->parent == nullptr) return;
		auto* parent = focused->parent;
		equalizeRecursive(parent, false);
		target = parent;
	}

	if (target != nullptr) {
		target->recalcSizePosRecursive();
	}
}

void Hy3Layout::warpCursorToBox(const Vector2D& pos, const Vector2D& size) {
	auto cursorpos = g_pPointerManager->position();

	if (cursorpos.x < pos.x || cursorpos.x >= pos.x + size.x || cursorpos.y < pos.y
	    || cursorpos.y >= pos.y + size.y)
	{
		Hy3Layout::warpCursorWithFocus(pos + size / 2, true);
	}
}

void Hy3Layout::warpCursorWithFocus(const Vector2D& target, bool force) {
	static const auto input_follows_mouse = ConfigValue<Hyprlang::INT>("input:follow_mouse");
	static const auto no_warps = ConfigValue<Hyprlang::INT>("cursor:no_warps");

	g_pCompositor->warpCursorTo(target, force);

	if (*no_warps && !force) return;

	if (*input_follows_mouse) {
		g_pInputManager->simulateMouseMovement();
	}
}

bool Hy3Layout::shouldRenderSelected(const CWindow* window) {
	if (window == nullptr) return false;
	auto* root = this->getWorkspaceRootGroup(window->m_workspace.get());
	if (root == nullptr || root->data.as_group().focused_child == nullptr) return false;
	auto* focused = root->getFocusedNode();
	if (focused == nullptr
	    || (focused->data.is_window()
	        && focused->data.as_window() != Desktop::focusState()->window()))
		return false;

	switch (focused->data.type()) {
	case Hy3NodeType::Window: return focused->data.as_window().get() == window;
	case Hy3NodeType::Group: {
		auto* node = this->getNodeFromWindow(window);
		if (node == nullptr) return false;
		return focused->data.as_group().hasChild(node);
	}
	}
	return false;
}

Hy3Node* Hy3Layout::getWorkspaceRootGroup(const CWorkspace* workspace) {
	for (auto& node: this->nodes) {
		if (node.workspace.get() == workspace && node.parent == nullptr && node.data.is_group()
		    && !node.reparenting)
		{
			return &node;
		}
	}

	return nullptr;
}

Hy3Node* Hy3Layout::getWorkspaceFocusedNode(
    const CWorkspace* workspace,
    bool ignore_group_focus,
    bool stop_at_expanded
) {
	auto* rootNode = this->getWorkspaceRootGroup(workspace);
	if (rootNode == nullptr) return nullptr;
	return rootNode->getFocusedNode(ignore_group_focus, stop_at_expanded);
}

void Hy3Layout::renderHook(void*, SCallbackInfo&, std::any data) {
	static bool rendering_normally = false;
	static std::vector<Hy3TabGroup*> rendered_groups;

	auto render_stage = std::any_cast<eRenderStage>(data);

	switch (render_stage) {
	case RENDER_PRE_WINDOWS:
		rendering_normally = true;
		rendered_groups.clear();
		break;
	case RENDER_POST_WINDOW:
		if (!rendering_normally) break;

		for (auto& entry: Hy3Layout::tab_groups) {
			if (!entry.hidden && entry.target_window == g_pHyprOpenGL->m_renderData.currentWindow.lock()
			    && std::find(rendered_groups.begin(), rendered_groups.end(), &entry)
			           == rendered_groups.end())
			{
				g_pHyprRenderer->m_renderPass.add(makeUnique<Hy3TabPassElement>(&entry));
				rendered_groups.push_back(&entry);
			}
		}

		break;
	case RENDER_POST_WINDOWS:
		rendering_normally = false;
		break;
	default: break;
	}
}

void Hy3Layout::windowGroupUrgentHook(void* p, SCallbackInfo& callback_info, std::any data) {
	auto window = std::any_cast<PHLWINDOW>(data);
	if (window == nullptr) return;
	window->m_isUrgent = true;
	Hy3Layout::windowGroupUpdateRecursiveHook(p, callback_info, data);
}

void Hy3Layout::windowGroupUpdateRecursiveHook(void*, SCallbackInfo&, std::any data) {
	auto window = std::any_cast<PHLWINDOW>(data);
	if (window == nullptr) return;
	auto* layout = Hy3Layout::getLayoutForWorkspace(window->m_workspace.get());
	if (!layout) return;
	auto* node = layout->getNodeFromWindow(window.get());

	if (node == nullptr) return;
	node->updateTabBarRecursive();
}

void Hy3Layout::tickHook(void*, SCallbackInfo&, std::any) {
	auto& tg = Hy3Layout::tab_groups;
	auto entry = tg.begin();
	while (entry != tg.end()) {
		entry->tick();
		if (entry->bar.destroy) tg.erase(entry++);
		else entry = std::next(entry);
	}
}

void Hy3Layout::mouseButtonHook(void*, SCallbackInfo& info, std::any data) {
	auto event = std::any_cast<IPointer::SButtonEvent>(data);
	if (event.state != 1 || event.button != 272) return;

	auto ptr_surface_resource = g_pSeatManager->m_state.pointerFocus.lock();
	if (!ptr_surface_resource) return;

	auto ptr_surface = CWLSurface::fromResource(ptr_surface_resource);
	if (!ptr_surface) return;

	// non window-parented surface focused, cant have a tab
	auto view = ptr_surface->view();
	auto* window = dynamic_cast<CWindow*>(view.get());
	if (!window || window->m_isFloating || window->isFullscreen()) return;

	auto* layout = Hy3Layout::getLayoutForWorkspace(window->m_workspace.get());
	if (!layout) return;
	auto* node = layout->getNodeFromWindow(window);
	if (!node) return;

	auto* root = node;
	while (root->parent) root = root->parent;

	Hy3Node* focus = nullptr;
	auto mouse_pos = g_pInputManager->getMouseCoordsInternal();
	auto* tab_node = findTabBarAt(*root, mouse_pos, &focus);
	if (!tab_node) return;

	while (focus->data.is_group() && !focus->data.as_group().group_focused
	       && focus->data.as_group().focused_child != nullptr)
		focus = focus->data.as_group().focused_child;

	focus->focus(false);
	g_pInputManager->simulateMouseMovement();
	tab_node->recalcSizePosRecursive();

	info.cancelled = true;
}

void Hy3Layout::activeWindowHook(void*, SCallbackInfo&, std::any data) {
	auto window = std::any_cast<PHLWINDOW>(data);
	if (!window) return;
	auto* layout = Hy3Layout::getLayoutForWorkspace(window->m_workspace.get());
	if (!layout) return;
	layout->onWindowFocusChange(window);
}

Hy3Node* Hy3Layout::getNodeFromWindow(const CWindow* window) {
	for (auto& node: this->nodes) {
		if (node.data.is_window() && node.data.as_window().get() == window) {
			return &node;
		}
	}

	return nullptr;
}

void Hy3Layout::applyNodeDataToWindow(Hy3Node* node, bool no_animation) {
	if (node->data.is_group()) return;
	auto window = node->data.as_window();
	auto root_node = this->getWorkspaceRootGroup(window->m_workspace.get());

	auto& monitor = node->workspace->m_monitor;

	if (monitor == nullptr) {
		hy3_log(
		    ERR,
		    "node {:x}'s workspace has no associated monitor, cannot apply node data",
		    (uintptr_t) node
		);
		errorNotif();
		return;
	}

	static const auto no_gaps_when_only = ConfigValue<Hyprlang::INT>("plugin:hy3:no_gaps_when_only");

	if (!valid(window) || !window->m_isMapped) {
		hy3_log(
		    ERR,
		    "node {:x} is an unmapped window ({:x}), cannot apply node data, removing from tiled "
		    "layout",
		    (uintptr_t) node,
		    (uintptr_t) window.get()
		);
		errorNotif();
		this->onWindowRemovedTiling(window);
		return;
	}

	window->m_ruleApplicator->resetProps(Desktop::Rule::RULE_PROP_ALL, Desktop::Types::PRIORITY_LAYOUT);
	window->updateWindowData();

	auto nodeBox = CBox(node->position, node->size);
	nodeBox.round();

	window->m_size = nodeBox.size();
	window->m_position = nodeBox.pos();

	window->updateWindowDecos();

	auto only_node = root_node != nullptr && root_node->data.as_group().children.size() == 1
	              && root_node->data.as_group().children.front()->data.is_window();

	if (!window->m_workspace->m_isSpecialWorkspace
	    && ((*no_gaps_when_only != 0 && (only_node || window->isFullscreen()))
	        || window->isEffectiveInternalFSMode(FSMODE_FULLSCREEN)))
	{

		const auto reserved = window->getFullWindowReservedArea();

		*window->m_realPosition = window->m_position + reserved.topLeft;
		*window->m_realSize = window->m_size - (reserved.topLeft + reserved.bottomRight);
	} else {
		auto reserved = window->getFullWindowReservedArea();
		auto wb = node->getStandardWindowArea({-reserved.topLeft, -reserved.bottomRight});

		*window->m_realPosition = wb.pos();
		*window->m_realSize = wb.size();

		if (no_animation) {
			g_pHyprRenderer->damageWindow(window);

			window->m_realPosition->warp();
			window->m_realSize->warp();

			g_pHyprRenderer->damageWindow(window);
		}

		window->updateWindowDecos();
	}

	window->m_workspace->updateWindows();
}

bool shiftIsForward(ShiftDirection direction) {
	return direction == ShiftDirection::Right || direction == ShiftDirection::Down;
}

bool shiftIsVertical(ShiftDirection direction) {
	return direction == ShiftDirection::Up || direction == ShiftDirection::Down;
}

bool shiftMatchesLayout(Hy3GroupLayout layout, ShiftDirection direction) {
	return (layout == Hy3GroupLayout::SplitV && shiftIsVertical(direction))
	    || (layout != Hy3GroupLayout::SplitV && !shiftIsVertical(direction));
}

Hy3Node* Hy3Layout::shiftOrGetFocus(
    Hy3Node* node,
    ShiftDirection direction,
    bool shift,
    bool once,
    bool visible
) {
	node = &node->getExpandActor();
	auto* break_origin = &node->getPlacementActor();
	auto* shift_actor = break_origin;
	auto* break_parent = break_origin->parent;

	auto has_broken_once = false;

	// break parents until we hit a container oriented the same way as the shift
	// direction
	while (true) {
		if (break_parent == nullptr) return nullptr;

		auto& group = break_parent->data.as_group(); // must be a group in order to be a parent

		if (shiftMatchesLayout(group.layout, direction)
		    && (!visible || group.layout != Hy3GroupLayout::Tabbed))
		{
			// group has the correct orientation

			if (once && shift && has_broken_once) break;
			if (break_origin != shift_actor) has_broken_once = true;

			// if this movement would break out of the group, continue the break loop
			// (do not enter this if) otherwise break.
			if ((has_broken_once && once && shift)
			    || !(
			        (!shiftIsForward(direction) && group.children.front() == break_origin)
			        || (shiftIsForward(direction) && group.children.back() == break_origin)
			    ))
				break;
		}

		if (break_parent->parent == nullptr) {
			if (!shift) return focusMonitor(direction);

			// if we haven't gone up any levels and the group is in the same direction
			// there's no reason to wrap the root group.
			if (group.layout != Hy3GroupLayout::Tabbed && shiftMatchesLayout(group.layout, direction)) {
				break;
			}

			if (group.layout != Hy3GroupLayout::Tabbed && group.children.size() == 2
			    && std::find(group.children.begin(), group.children.end(), shift_actor)
			           != group.children.end())
			{
				group.setLayout(
				    shiftIsVertical(direction) ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH
				);
			} else {
				// wrap the root group in another group
				this->nodes.push_back({
				    .parent = break_parent,
				    .data = shiftIsVertical(direction) ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
				    .position = break_parent->position,
				    .size = break_parent->size,
				    .workspace = break_parent->workspace,
				    .layout = this,
				});

				auto* newChild = &this->nodes.back();
				Hy3Node::swapData(*break_parent, *newChild);
				auto& group = break_parent->data.as_group();
				group.children.push_back(newChild);
				group.group_focused = false;
				group.focused_child = newChild;
				break_origin = newChild;
			}

			break;
		} else {
			break_origin = break_parent;
			break_parent = break_origin->parent;
		}
	}

	auto& parent_group = break_parent->data.as_group();
	Hy3Node* target_group = break_parent;
	std::list<Hy3Node*>::iterator insert;

	if (break_origin == parent_group.children.front() && !shiftIsForward(direction)) {
		if (!shift) return nullptr;
		insert = parent_group.children.begin();
	} else if (break_origin == parent_group.children.back() && shiftIsForward(direction)) {
		if (!shift) return nullptr;
		insert = parent_group.children.end();
	} else {
		auto& group_data = target_group->data.as_group();

		auto iter = std::find(group_data.children.begin(), group_data.children.end(), break_origin);
		if (shiftIsForward(direction)) iter = std::next(iter);
		else iter = std::prev(iter);

		if ((*iter)->data.is_window()
		    || ((*iter)->data.is_group()
		        && ((*iter)->data.as_group().expand_focused != ExpandFocusType::NotExpanded
		            || (*iter)->data.as_group().locked))
		    || (shift && once && has_broken_once))
		{
			if (shift) {
				if (target_group == shift_actor->parent) {
					if (shiftIsForward(direction)) insert = std::next(iter);
					else insert = iter;
				} else {
					if (shiftIsForward(direction)) insert = iter;
					else insert = std::next(iter);
				}
			} else return (*iter)->getFocusedNode();
		} else {
			// break into neighboring groups until we hit a window
			while (true) {
				target_group = *iter;
				auto& group_data = target_group->data.as_group();

				if (group_data.children.empty()) return nullptr; // in theory this would never happen

				bool shift_after = false;

				if (!shift && group_data.layout == Hy3GroupLayout::Tabbed
				    && group_data.focused_child != nullptr)
				{
					iter = std::find(
					    group_data.children.begin(),
					    group_data.children.end(),
					    group_data.focused_child
					);
				} else if (visible && group_data.layout == Hy3GroupLayout::Tabbed
				           && group_data.focused_child != nullptr)
				{
					// if the group is tabbed and we're going by visible nodes, jump to the current entry
					iter = std::find(
					    group_data.children.begin(),
					    group_data.children.end(),
					    group_data.focused_child
					);
					shift_after = true;
				} else if (shiftMatchesLayout(group_data.layout, direction)
				           || (visible && group_data.layout == Hy3GroupLayout::Tabbed))
				{
					// if the group has the same orientation as movement pick the
					// last/first child based on movement direction
					if (shiftIsForward(direction)) iter = group_data.children.begin();
					else {
						iter = std::prev(group_data.children.end());
						shift_after = true;
					}
				} else {
					if (group_data.focused_child != nullptr) {
						iter = std::find(
						    group_data.children.begin(),
						    group_data.children.end(),
						    group_data.focused_child
						);
						shift_after = true;
					} else {
						iter = group_data.children.begin();
					}
				}

				if (shift && once) {
					if (shift_after) insert = std::next(iter);
					else insert = iter;
					break;
				}

				if ((*iter)->data.is_window()
				    || ((*iter)->data.is_group()
				        && (*iter)->data.as_group().expand_focused != ExpandFocusType::NotExpanded))
				{
					if (shift) {
						if (shift_after) insert = std::next(iter);
						else insert = iter;
						break;
					} else {
						return (*iter)->getFocusedNode();
					}
				}
			}
		}
	}

	auto& group_data = target_group->data.as_group();

	if (target_group == shift_actor->parent) {
		// nullptr is used as a signal value instead of removing it first to avoid
		// iterator invalidation.
		auto iter = std::find(group_data.children.begin(), group_data.children.end(), shift_actor);
		*iter = nullptr;
		auto& group = target_group->data.as_group();
		group.children.insert(insert, shift_actor);
		group.children.remove(nullptr);
		target_group->recalcSizePosRecursive();
	} else {
		target_group->data.as_group().children.insert(insert, shift_actor);

		// must happen AFTER `insert` is used
		auto* old_parent = shift_actor->removeFromParentRecursive(nullptr);
		shift_actor->parent = target_group;
		shift_actor->size_ratio = 1.0;

		if (old_parent != nullptr) {
			auto& group = old_parent->data.as_group();
			if (old_parent->parent != nullptr && group.ephemeral && group.children.size() == 1
			    && !group.hasChild(shift_actor))
			{
				Hy3Node::swallowGroups(old_parent);
			}

			old_parent->updateTabBarRecursive();
			old_parent->recalcSizePosRecursive();
		}

		target_group->recalcSizePosRecursive();

		auto* target_parent = target_group->parent;
		while (target_parent != nullptr && Hy3Node::swallowGroups(target_parent)) {
			target_parent = target_parent->parent;
		}

		node->updateTabBarRecursive();
		node->focus(false);

		if (target_parent != target_group && target_parent != nullptr)
			target_parent->recalcSizePosRecursive();
	}

	return nullptr;
}

void Hy3Layout::updateAutotileWorkspaces() {
	static const auto autotile_raw_workspaces =
	    ConfigValue<Hyprlang::STRING>("plugin:hy3:autotile:workspaces");

	if (*autotile_raw_workspaces == this->autotile.raw_workspaces) {
		return;
	}

	this->autotile.raw_workspaces = *autotile_raw_workspaces;
	this->autotile.workspaces.clear();

	if (this->autotile.raw_workspaces == "all") {
		return;
	}

	this->autotile.workspace_blacklist = this->autotile.raw_workspaces.rfind("not:", 0) == 0;

	const auto autotile_raw_workspaces_filtered = (this->autotile.workspace_blacklist)
	                                                ? this->autotile.raw_workspaces.substr(4)
	                                                : this->autotile.raw_workspaces;

	// split on space and comma
	const std::regex regex {R"([\s,]+)"};
	const auto begin = std::sregex_token_iterator(
	    autotile_raw_workspaces_filtered.begin(),
	    autotile_raw_workspaces_filtered.end(),
	    regex,
	    -1
	);
	const auto end = std::sregex_token_iterator();

	for (auto s = begin; s != end; ++s) {
		try {
			this->autotile.workspaces.insert(std::stoi(*s));
		} catch (...) {
			hy3_log(ERR, "autotile:workspaces: invalid workspace id: {}", (std::string) *s);
		}
	}
}

bool Hy3Layout::shouldAutotileWorkspace(const CWorkspace* workspace) {
	if (this->autotile.workspace_blacklist) {
		return !this->autotile.workspaces.contains(workspace->m_id);
	} else {
		return this->autotile.workspaces.empty()
		    || this->autotile.workspaces.contains(workspace->m_id);
	}
}
