#include <Geode/Geode.hpp>
#include <unordered_map>

using namespace geode::prelude;

#include <Geode/modify/CCTouchDispatcher.hpp>

struct FuckTouchDispatcher : Modify<FuckTouchDispatcher, CCTouchDispatcher> {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCTouchDispatcher::touches", Priority::Replace);
    }

    template <std::derived_from<CCTouchHandler> Handler>
    struct HandlerIterable {
        std::unordered_map<CCNode*, Handler*> handler_map;
        std::unordered_map<CCNode*, std::vector<CCNode*>> children_map;
        std::unordered_map<CCNode*, CCNode*> sibling_map;

        HandlerIterable(CCArrayExt<Handler*> handlers) {
            for (auto handler : handlers) {
                if (!handler) continue;
                auto delegate = handler->getDelegate();
                if (!delegate) continue;
                auto node = typeinfo_cast<CCNode*>(delegate);
                if (!node) continue;
                addPath(node);
                // if someone inherits from CCTouchDelegate more than once, it's very much their problem
                handler_map[node] = handler;
            }
        }

        // we treat nullptr as the root of roots
        void addPath(CCNode* node) {
            if (children_map.contains(node) || handler_map.contains(node) || !node) return;
            auto parent = node->getParent();
            addPath(parent);
            children_map[parent].push_back(node);
        }

        struct iterator {
            using difference_type = std::ptrdiff_t;
            using value_type = Handler*;

            CCNode* node{nullptr};
            HandlerIterable* iterable;

            iterator(HandlerIterable* iterable) : iterable{iterable} {
                goDown();
            }

            // if we're going down, since all leaves of the "relevant" subgraph are delegates, we won't go up again
            void goDown() {
                while (iterable->children_map.contains(node)) {
                    auto& children = iterable->children_map[node];
                    // lazily sort and link siblings only when needed
                    std::ranges::sort(children, [](CCNode* a, CCNode* b) {
                        if (a->getZOrder() != b->getZOrder()) return a->getZOrder() > b->getZOrder();
                        return a->getOrderOfArrival() > b->getOrderOfArrival();
                    });
                    auto it2 = children.begin();
                    decltype(it2) it1;
                    // we know children is nonempty
                    while (it1 = it2, ++it2 != children.end()) {
                        iterable->sibling_map[*it1] = *it2;
                    }
                    node = children.front();
                }
            }

            // postfix traversal of the "relevant" subgraph, directly moving to siblings instead of going parent->child
            // (to make it easy to iterate over siblings in the right order)
            iterator& operator++() {
                // postfix so we must be going up
                while (!iterable->sibling_map.contains(node)) {
                    node = node->getParent();
                    if (!node || iterable->handler_map.contains(node)) return *this;
                }
                node = iterable->sibling_map[node];
                goDown();
                return *this;
            }

            Handler* operator*() const { return iterable->handler_map[node]; }

            bool operator==(std::nullptr_t) const { return !node; }

            void operator++(int) { ++*this; }
        };
        static_assert(std::input_iterator<iterator>);

        iterator begin() { return {this}; }

        std::nullptr_t end() { return nullptr; }
    };

    void handleTargetedHandlers(CCSet* touches, CCEvent* event, unsigned int index) {
        auto handlers = HandlerIterable<CCTargetedTouchHandler>(m_pTargetedHandlers);

        std::vector<CCTouch*> touchesCopy;
        for (auto touch : *touches) {
            touchesCopy.push_back(static_cast<CCTouch*>(touch));
        }

        for (auto touch : touchesCopy) {
            for (auto handler : handlers) {
                auto delegate = handler->getDelegate();
                auto claimedTouches = handler->m_pClaimedTouches;
                auto swallowsTouches = handler->m_bSwallowsTouches;

                bool claimed = false;
                if (index == CCTOUCHBEGAN) {
                    claimed = delegate->ccTouchBegan(touch, event);

                    if (claimed) {
                        auto node = typeinfo_cast<CCNode*>(delegate);
                        log::debug("Node {}({}) claimed touch", node, node->getID());
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
        auto handlers = HandlerIterable<CCStandardTouchHandler>(m_pStandardHandlers);

        for (auto handler : handlers) {
            auto delegate = handler->getDelegate();

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
