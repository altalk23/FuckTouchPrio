#include "Geode/cocos/touch_dispatcher/CCTouchDispatcher.h"
#include "Geode/cocos/touch_dispatcher/CCTouchHandler.h"
#include "Geode/loader/Event.hpp"
#include "Geode/loader/SettingV3.hpp"
#include <Geode/Geode.hpp>
#include <memory>
#include <unordered_map>

using namespace geode::prelude;

static bool s_enableDebugLogs = false;
static auto _ = new EventListener<SettingChangedFilterV3>(+[](std::shared_ptr<SettingV3> event) {
    if (auto boolSetting = typeinfo_cast<BoolSettingV3*>(event.get())) {
        if (boolSetting->getKey() == "enable-debug-logs") {
            s_enableDebugLogs = boolSetting->getValue();
            log::info("Debug logs {}", s_enableDebugLogs ? "enabled" : "disabled");
        }
    }
}, SettingChangedFilterV3(Mod::get(), "enable-debug-logs"));

#include <Geode/modify/CCTouchDispatcher.hpp>

struct FuckTouchDispatcher : Modify<FuckTouchDispatcher, CCTouchDispatcher> {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCTouchDispatcher::touches", Priority::Replace);
    }

    template <class Handler>
    struct ParentPath {
        std::vector<CCNode*> path;
        Handler* handler = nullptr;

        ParentPath(CCNode* node, Handler* handler) : handler(handler) {
            while (node) {
                path.push_back(node);
                node = node->getParent();
            }
        }

        CCNode* leaf() const {
            return path.empty() ? nullptr : path.front();
        }

        CCNode* root() const {
            return path.empty() ? nullptr : path.back();
        }

        CCNode* nth(size_t n) const {
            return (n < path.size()) ? path[path.size() - 1 - n] : nullptr;
        }

        bool swallows() const {
            if constexpr (std::is_same_v<Handler, CCTargetedTouchHandler>) {
                return handler->m_bSwallowsTouches;
            }
            return false;
        }

        // assumes all nodes will converge at the same root
        bool operator<(ParentPath const& other) const {
            if (this->root() != other.root()) {
                return this->root() < other.root(); // different roots, cannot compare
            }

            size_t maxLength = std::max(path.size(), other.path.size());
            // nth(1) gives the child of the root, which is fine since we already compared the root
            for (size_t divergeIndex = 1; divergeIndex < maxLength; ++divergeIndex) {
                auto thisParent = this->nth(divergeIndex);
                auto otherParent = other.nth(divergeIndex);

                // okay so i know this is complicated
                // and yes it is but basically
                // lets have 2 nodes, both respond to touch
                // one is a parent to other, lets call parent and child
                // if parent doesnt swallow the touch, in gd usually what that means is
                // both parent and child can get the touch, and usually parent "steals"
                // the touch from the child whenever necessary
                // at least thats how scroll layers kind of work in gd
                // this is what its trying to emulate basically

                // swallows like a good boy >w<\n
                // https://github.com/geode-sdk/api/commit/01bcacde38a225edb51a06961ce6553851dd24c5

                if (!thisParent && otherParent) {
                    // other is deeper in compared to this, other should come first
                    // if we dont swallow touches we can allow it to come after us though
                    // we swallow: false
                    // we dont and they swallow: true
                    // both dont swallow: false
                    return !this->swallows() && other.swallows();
                }
                else if (!otherParent && thisParent) {
                    // this is deeper in compared to other, this should come first
                    // if other doesnt swallow touches we can allow it to come before us though
                    // they swallow: true
                    // they dont and we do: false
                    // both dont swallow: true
                    return other.swallows() || !this->swallows();
                }
                else if (thisParent != otherParent) {
                    // otherwise we dont care anyway
                    
                    // higher Z order should come first
                    if (thisParent->getZOrder() == otherParent->getZOrder()) {
                        // same Z order, use order of arrival
                        return thisParent->getOrderOfArrival() > otherParent->getOrderOfArrival();
                    }
                    return thisParent->getZOrder() > otherParent->getZOrder();
                }
            }
            // reached the leaves together without diverging, same node?
            return false;
        }
    };

    template <class Handler>
    std::vector<ParentPath<Handler>> getRegisteredPaths(CCArray* handlers) const {
        std::vector<ParentPath<Handler>> paths;
        for (auto handler : CCArrayExt<Handler*>(handlers)) {
            if (!handler) continue;
            auto delegate = handler->getDelegate();
            if (!delegate) continue;
            auto node = typeinfo_cast<CCNode*>(delegate);
            if (!node) continue;

            paths.emplace_back(node, handler);
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    void handleTargetedHandlers(CCSet* touches, CCEvent* event, unsigned int index) {
        auto registeredPaths = this->getRegisteredPaths<CCTargetedTouchHandler>(m_pTargetedHandlers);

        std::vector<CCTouch*> touchesCopy;
        for (auto touch : *touches) {
            touchesCopy.push_back(static_cast<CCTouch*>(touch));
        }

        for (auto touch : touchesCopy) {
            for (auto& path : registeredPaths) {
                auto delegate = path.handler->getDelegate();
                auto claimedTouches = path.handler->m_pClaimedTouches;
                auto swallowsTouches = path.handler->m_bSwallowsTouches;

                bool claimed = false;
                if (index == CCTOUCHBEGAN) {
                    claimed = delegate->ccTouchBegan(touch, event);

                    if (claimed) {
                        if (s_enableDebugLogs) log::debug("Node {}({}) claimed touch", path.leaf(), path.leaf()->getID());
                        claimedTouches->addObject(touch);
                    }
                } 
                else if (claimedTouches->containsObject(touch)) {
                    // moved ended canceled
                    claimed = true;

                    switch (m_sHandlerHelperData[index].m_type) {
                    case CCTOUCHMOVED:
                        delegate->ccTouchMoved(touch, event);
                        break;
                    case CCTOUCHENDED:
                        delegate->ccTouchEnded(touch, event);
                        claimedTouches->removeObject(touch);
                        break;
                    case CCTOUCHCANCELLED:
                        delegate->ccTouchCancelled(touch, event);
                        claimedTouches->removeObject(touch);
                        break;
                    }
                }

                if (claimed && swallowsTouches) {
                    touches->removeObject(touch);

                    break;
                }
            }
        }
    }

    void handleStandardHandlers(CCSet* touches, CCEvent* event, unsigned int index) {
        auto registeredPaths = this->getRegisteredPaths<CCStandardTouchHandler>(m_pStandardHandlers);

        for (auto& path : registeredPaths) {
            auto delegate = path.handler->getDelegate();

            switch (m_sHandlerHelperData[index].m_type) {
            case CCTOUCHBEGAN:
                delegate->ccTouchesBegan(touches, event);
                break;
            case CCTOUCHMOVED:
                delegate->ccTouchesMoved(touches, event);
                break;
            case CCTOUCHENDED:
                delegate->ccTouchesEnded(touches, event);
                break;
            case CCTOUCHCANCELLED:
                delegate->ccTouchesCancelled(touches, event);
                break;
            }
        }
    }

    $override
    void touches(CCSet* touches, CCEvent* event, unsigned int index) {
        m_bLocked = true;

        //
        // process the target handlers 1st
        //
        if (m_pTargetedHandlers->count() > 0) {
            this->handleTargetedHandlers(touches, event, index);
        }
        //
        // process standard handlers 2nd
        //
        if (m_pStandardHandlers->count() > 0 && touches->m_pSet->size() > 0) {
            this->handleStandardHandlers(touches, event, index);
        }

        //
        // Optimization. To prevent a [handlers copy] which is expensive
        // the add/removes/quit is done after the iterations
        //
        m_bLocked = false;
        if (m_bToRemove) {
            m_bToRemove = false;
            for (unsigned int i = 0; i < m_pHandlersToRemove->num; ++i) {
                for (auto handler : CCArrayExt<CCTargetedTouchHandler*>(m_pTargetedHandlers)) {
                    if (handler->getDelegate() == m_pHandlersToRemove->arr[i]) {
                        m_pTargetedHandlers->removeObject(handler);
                        break;
                    }
                }
                for (auto handler : CCArrayExt<CCStandardTouchHandler*>(m_pStandardHandlers)) {
                    if (handler->getDelegate() == m_pHandlersToRemove->arr[i]) {
                        m_pStandardHandlers->removeObject(handler);
                        break;
                    }
                }
            }
            m_pHandlersToRemove->num = 0;
        }

        if (m_bToAdd) {
            m_bToAdd = false;
            for (auto handler : CCArrayExt<CCTouchHandler*>(m_pHandlersToAdd)) {
                if (!handler) continue;

                if (typeinfo_cast<CCTargetedTouchHandler*>(handler)) {
                    m_pTargetedHandlers->addObject(handler);
                }
                else {
                    m_pStandardHandlers->addObject(handler);
                }
            }
    
            m_pHandlersToAdd->removeAllObjects();    
        }

        if (m_bToQuit) {
            m_bToQuit = false;
            m_pStandardHandlers->removeAllObjects();
            m_pTargetedHandlers->removeAllObjects();
        }
    }
};

#include <Geode/modify/GJBaseGameLayer.hpp>

struct FuckEditorPrio : Modify<FuckEditorPrio, GJBaseGameLayer> {
    $override
    bool init() override {
        if (!GJBaseGameLayer::init()) return false;

        // Nice one robtop
        if (m_uiLayer) m_uiLayer->setZOrder(2);

        return true;
    }
};
