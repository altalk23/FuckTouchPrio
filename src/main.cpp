#include <Geode/Geode.hpp>
#include <Geode/utils/VMTHookManager.hpp>
#include <unordered_set>

using namespace geode::prelude;

static bool s_enableDebugLogs = Mod::get()->getSettingValue<bool>("enable-debug-logs");
$on_mod(Loaded) {
    SettingChangedEventV3(Mod::get(), "enable-debug-logs").listen([](std::shared_ptr<geode::SettingV3> setting) {
        s_enableDebugLogs = Mod::get()->getSettingValue<bool>("enable-debug-logs");
    }).leak();
}

static inline std::unordered_set<cocos2d::CCObject*> s_forcePrioObjects;

#include <Geode/modify/CCDirector.hpp>
struct FuckDirector : Modify<FuckDirector, CCDirector> {
    $override
    void setNextScene() {
        auto dispatcher = CCTouchDispatcher::get();
        if (dispatcher->m_forcePrio != 0) {
            log::error("Detected leaked force prio!!");
            log::error("Printing all leaked objects!!");
            for (auto obj : s_forcePrioObjects) {
                if (auto node = typeinfo_cast<CCNode*>(obj)) {
                    log::error("{}({})", node, node->getID());
                }
                else {
                    log::error("{}", obj);
                }
            }
        }
        CCDirector::setNextScene();
    }
};

#include <Geode/modify/CCTouchDispatcher.hpp>

struct FuckTouchDispatcher : Modify<FuckTouchDispatcher, CCTouchDispatcher> {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCTouchDispatcher::touches", Priority::Replace);
    }

    $override
    void registerForcePrio(cocos2d::CCObject* obj, int prio) {
        // log::debug("register {}", obj);
        s_forcePrioObjects.insert(obj);
        return CCTouchDispatcher::registerForcePrio(obj, prio);
    }

    $override
    void unregisterForcePrio(cocos2d::CCObject* obj) {
        // log::debug("unregister {}", obj);
        s_forcePrioObjects.erase(obj);
        return CCTouchDispatcher::unregisterForcePrio(obj);
    }

    template <class Handler>
    struct ParentPath {
        std::vector<CCNode*> path;
        Handler* handler = nullptr;
        mutable bool hasInvalidRoot = false;

        ParentPath(Handler* handler) : handler(handler) {}
        ParentPath(ParentPath&& other) = default;
        ParentPath& operator=(ParentPath&& other) = default;

        ParentPath(CCNode* node, Handler* handler) : handler(handler) {
            while (node) {
                path.push_back(node);
                node = node->getParent();
            }
        }

        static std::optional<ParentPath> filtered(CCNode* node, Handler* handler, CCNode* filter) {
            ParentPath ret{handler};
            bool confirmed = false;
            while (node) {
                ret.path.push_back(node);
                if (node == filter) confirmed = true;
                node = node->getParent();
            }
            if (confirmed) return ret;
            return std::nullopt;
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

        bool steals() const {
            // if it swallows, no need to check for stealing behavior
            if (this->swallows()) return false; 

            if constexpr (std::is_same_v<Handler, CCTargetedTouchHandler>) {
                if (auto leaf = this->leaf()) {
                    // the stealers, might think of a more general way of doing this later
                    if (typeinfo_cast<TableView*>(leaf) || typeinfo_cast<BoomScrollLayer*>(leaf)) return true;
                    // custom stealer:
                    if (auto scrollLayer = typeinfo_cast<ScrollLayer*>(leaf)) {
                        return scrollLayer->isStealingTouches();
                    }
                }
            }
            return false;
        }

        bool compareRoots(ParentPath const& other) const {
            // same root, not less than
            if (this->root() == other.root()) return false; 
            
            auto director = CCDirector::get();
            auto const rootOrder = std::array<CCNode*, 2>{director->m_pRunningScene, director->m_pNotificationNode};
            auto thisRootIt = std::find(rootOrder.begin(), rootOrder.end(), this->root());
            auto otherRootIt = std::find(rootOrder.begin(), rootOrder.end(), other.root());

            if (thisRootIt == rootOrder.end()) {
                // If either root is not in the known order, we can't compare them reliably
                this->hasInvalidRoot = true;
                return false;
            }
            if (otherRootIt == rootOrder.end()) {
                other.hasInvalidRoot = true;
                return false;
            }

            // if this is running scene and other is notification node, other will be 1 and this will be 0
            // meaning other should come first, so we return false
            // if this is notification node and other is running scene, other will be 0 and this will be 1
            // meaning this should come first, so we return true
            return thisRootIt > otherRootIt;
        }

        // assumes all nodes will converge at the same root
        bool operator<(ParentPath const& other) const {
            if (this->root() != other.root()) {
                return this->compareRoots(other);
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
                    // we are the parent, they are the child
                    // if we dont steal touches we can allow it to come after us though
                    // we steal: true, we are first
                    // if there are nested stealers we dont handle that right now, 
                    // and im sure rob doesn't either, but if it ever happens
                    // shoot me a message 
                    return this->steals();
                }
                else if (!otherParent && thisParent) {
                    // this is deeper in compared to other, this should come first
                    // they are the parent, we are the child
                    // if other doesnt steal touches we can allow it to come before us though
                    // they steal: false, they are first
                    return !other.steals();
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
    std::vector<ParentPath<Handler>> getRegisteredPaths(CCArray* handlers, std::optional<CCNode*> filter) const {
        std::vector<ParentPath<Handler>> paths;
        for (auto handler : CCArrayExt<Handler*>(handlers)) {
            if (!handler) continue;
            auto delegate = handler->getDelegate();
            if (!delegate) continue;
            auto node = typeinfo_cast<CCNode*>(delegate);
            if (!node) continue;

            if (filter) {
                auto filtered = ParentPath<Handler>::filtered(node, handler, *filter);
                if (filtered) {
                    paths.push_back(std::move(filtered.value()));
                }
            }
            else {
                paths.emplace_back(node, handler);
            }
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    template <class Handler>
    bool handleSingleTargetedHandlers(CCSet* touches, CCTouch* touch, CCEvent* event, unsigned int index, std::vector<ParentPath<Handler>> const& registeredPaths) {
        bool touchClaimed = false;
        if (index == CCTOUCHBEGAN) for (auto& path : registeredPaths) {
            if (path.hasInvalidRoot) {
                log::error("Handler {}({}) has an invalid root, detected leaked node!! Please report the issue to the owner of the node!!", path.leaf(), path.leaf()->getID());
            }
        }

        for (auto& path : registeredPaths) {
            auto delegate = path.handler->getDelegate();
            auto claimedTouches = path.handler->m_pClaimedTouches;
            auto swallowsTouches = path.handler->m_bSwallowsTouches;

            if (path.hasInvalidRoot) continue;

            bool claimed = false;
            if (index == CCTOUCHBEGAN) {
                claimed = delegate->ccTouchBegan(touch, event);

                if (claimed) {
                    if (s_enableDebugLogs) {
                        log::debug("Node {}({}) claimed touch", path.leaf(), path.leaf()->getID());
                        // for (auto node : path.path) {
                        //     log::debug("{} - {}", node, node->getID());
                        // }
                    }
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
                touchClaimed = true;
                if (touches) touches->removeObject(touch);
                break;
            }
        }
        return touchClaimed;
    }

    void handleTargetedHandlers(CCSet* touches, CCEvent* event, unsigned int index, std::optional<CCNode*> filter = std::nullopt) {
        auto registeredPaths = this->getRegisteredPaths<CCTargetedTouchHandler>(m_pTargetedHandlers, filter);

        std::vector<CCTouch*> touchesCopy;
        for (auto touch : *touches) {
            touchesCopy.push_back(static_cast<CCTouch*>(touch));
        }

        for (auto touch : touchesCopy) {
            this->handleSingleTargetedHandlers(touches, touch, event, index, registeredPaths);
        }
    }

    void handleStandardHandlers(CCSet* touches, CCEvent* event, unsigned int index) {
        auto registeredPaths = this->getRegisteredPaths<CCStandardTouchHandler>(m_pStandardHandlers, std::nullopt);

        for (auto& path : registeredPaths) {
            auto delegate = path.handler->getDelegate();

            if (s_enableDebugLogs) log::debug("Node {}({}) is standard handler", path.leaf(), path.leaf()->getID());

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
                    // crashes here indexing into m_pHandlesToRemove->arr (this+72 +16 +i*4)
                    // i == 0 in all crashlogs (r9)
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

    bool handleSingleTargetedHandlersWithFilter(CCTouch* touch, CCEvent* event, unsigned int index, CCNode* filter) {
        auto registeredPaths = this->getRegisteredPaths<CCTargetedTouchHandler>(m_pTargetedHandlers, filter);

        return this->handleSingleTargetedHandlers(nullptr, touch, event, index, registeredPaths);
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

class ObjectLayerTouchListener: public CCLayer {
public:
    CCLayer* m_objectLayer;

    static ObjectLayerTouchListener* create(CCLayer* objectLayer) {
        auto ret = new ObjectLayerTouchListener;
        if (ret->init(objectLayer)) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }

    bool init(CCLayer* objectLayer) {
        if (!CCLayer::init()) return false;

        m_objectLayer = objectLayer;

        this->setTouchEnabled(true);
        this->setID("object-layer-touch-listener"_spr);

        return true;
    }

    void registerWithTouchDispatcher() override {
        CCTouchDispatcher::get()->addTargetedDelegate(this, 0, true);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        return static_cast<FuckTouchDispatcher*>(CCTouchDispatcher::get())->handleSingleTargetedHandlersWithFilter(touch, event, CCTOUCHBEGAN, m_objectLayer);
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        static_cast<FuckTouchDispatcher*>(CCTouchDispatcher::get())->handleSingleTargetedHandlersWithFilter(touch, event, CCTOUCHMOVED, m_objectLayer);
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        static_cast<FuckTouchDispatcher*>(CCTouchDispatcher::get())->handleSingleTargetedHandlersWithFilter(touch, event, CCTOUCHENDED, m_objectLayer);
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        static_cast<FuckTouchDispatcher*>(CCTouchDispatcher::get())->handleSingleTargetedHandlersWithFilter(touch, event, CCTOUCHCANCELLED, m_objectLayer);
    }
};

#include <Geode/modify/EditorUI.hpp>
struct FuckEditorUI : Modify<FuckEditorUI, EditorUI> {
    $override
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        // below everything else in editorui, but inside editorui so it runs before editorui's touches
        this->addChild(ObjectLayerTouchListener::create(editorLayer->m_objectLayer), -1000); 

        return true;
    }
};
