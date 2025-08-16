#include <Geode/Geode.hpp>
#include <unordered_map>

using namespace geode::prelude;

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
                if (!thisParent && otherParent) {
                    // other is deeper in compared to this, other should come first
                    return false;
                }
                else if (!otherParent && thisParent) {
                    // this is deeper in compared to other, this should come first
                    return true;
                }
                else if (thisParent != otherParent) {
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
                        log::debug("Node {}({}) claimed touch", path.leaf(), path.leaf()->getID());
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

#include <Geode/modify/LevelEditorLayer.hpp>

struct FuckEditorPrioSequel : Modify<FuckEditorPrioSequel, LevelEditorLayer> {
    $override
    bool init(GJGameLevel* p0, bool p1) {
        if (!LevelEditorLayer::init(p0, p1)) return false;

        // Nice one robtop
        if (m_editorUI) m_editorUI->setZOrder(3);

        return true;
    }
};