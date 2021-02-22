
#include <map>
#include <algorithm>

#include <boost/algorithm/string.hpp>

#include "restc-cpp/RequestBuilder.h"

#include "k8deployer/AppComponent.h"
#include "k8deployer/Cluster.h"
#include "k8deployer/ClusterRoleBindingComponent.h"
#include "k8deployer/ClusterRoleComponent.h"
#include "k8deployer/Component.h"
#include "k8deployer/ConfigMapComponent.h"
#include "k8deployer/DaemonSetComponent.h"
#include "k8deployer/DeploymentComponent.h"
#include "k8deployer/Engine.h"
#include "k8deployer/IngressComponent.h"
#include "k8deployer/JobComponent.h"
#include "k8deployer/NamespaceComponent.h"
#include "k8deployer/PersistentVolumeComponent.h"
#include "k8deployer/RoleBindingComponent.h"
#include "k8deployer/RoleComponent.h"
#include "k8deployer/SecretComponent.h"
#include "k8deployer/ServiceAccountComponent.h"
#include "k8deployer/ServiceComponent.h"
#include "k8deployer/StatefulSetComponent.h"
#include "k8deployer/k8/k8api.h"
#include "k8deployer/logging.h"

using namespace std;
using namespace string_literals;
using namespace restc_cpp;

namespace k8deployer {

namespace {

const map<string, Kind> kinds = {{"App", Kind::APP},
                                 {"Job", Kind::JOB},
                                 {"Deployment", Kind::DEPLOYMENT},
                                 {"StatefulSet", Kind::STATEFULSET},
                                 {"Service", Kind::SERVICE},
                                 {"ConfigMap", Kind::CONFIGMAP},
                                 {"Secret", Kind::SECRET},
                                 {"PersitentVolume", Kind::PERSISTENTVOLUME},
                                 {"Ingress", Kind::INGRESS},
                                 {"Namespace", Kind::NAMESPACE},
                                 {"DaemonSet", Kind::DAEMONSET},
                                 {"Role", Kind::ROLE},
                                 {"ClusterRole", Kind::CLUSTERROLE},
                                 {"RoleBinding", Kind::ROLEBINDING},
                                 {"ClusterRoleBinding", Kind::CLUSTERROLEBINDING},
                                 {"ServiceAccount", Kind::SERVICEACCOUNT},
                                };
} // anonymous ns

// https://stackoverflow.com/questions/18816126/c-read-the-whole-file-in-buffer
string slurp (const string& path) {
  ostringstream buf;
  ifstream input (path.c_str());
  buf << input.rdbuf();
  return buf.str();
}


Kind toKind(const string &kind)
{
    return kinds.at(kind);
}

string toString(const Kind &kind)
{
    for(const auto& [k, v] : kinds) {
        if (kind == v) {
            return k;
        }
    }
    throw runtime_error("Unknown Kind enum value.");
}

void Component::init()
{
    setState(State::CREATING);
    effectiveArgs_ = mergeArgs();

    if (isRoot()) {
        if (Engine::config().autoMaintainNamespace) {
            auto ns = addChild(getNamespace() + "-ns", Kind::NAMESPACE);
            ns->namespace_.metadata.name = getNamespace();
        }
    }

    initChildren();
    validate();
}

std::string Base64Encode(const std::string &in) {
    // Silence the cursed clang-tidy...
    constexpr auto magic_4 = 4;
    constexpr auto magic_6 = 6;
    constexpr auto magic_8 = 8;
    constexpr auto magic_3f = 0x3F;

    static const string alphabeth {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    string out;

    int val = 0;
    int valb = -magic_6;
    for (const uint8_t c : in) {
        val = (val<<magic_8) + c;
        valb += magic_8;
        while (valb>=0) {
            out.push_back(alphabeth[(val>>valb)&magic_3f]);
            valb-=magic_6;
        }
    }
    if (valb>-magic_6) out.push_back(alphabeth[((val<<magic_8)>>(valb+magic_8))&magic_3f]);
    while (out.size()%magic_4) out.push_back('=');
    return out;
}

Component::Component(const Component::ptr_t &parent, Cluster &cluster, const ComponentData &data)
    : ComponentData(data), parent_{parent}, cluster_{&cluster},
      mode_{Engine::mode() == Engine::Mode::DELETE ? Mode::REMOVE : Mode::CREATE}
{
}

string Component::logName() const noexcept
{
    return cluster_->name() + "/" + toString(kind_) + "/" + name + " ";
}

std::optional<bool> Component::getBoolArg(const string &name) const
{
    if (auto it = effectiveArgs_.find(name) ; it != effectiveArgs_.end()) {
        if (it->second == "true" || it->second == "yes" || it->second == "1") {
            return true;
        }

        if (it->second == "false" || it->second == "no" || it->second == "0") {
            return false;
        }

        throw runtime_error("Argument "s + name + " is not a boolean value (1|0|true|false|yes|no)");
    }

    return {};
}

std::optional<string> Component::getArg(const string &name) const
{
    if (auto it = effectiveArgs_.find(name) ; it != effectiveArgs_.end()) {
        return it->second;
    }

    return {};
}

k8api::string_list_t Component::getArgAsStringList(const string &values, const string &defaultVal) const
{
    return getArgAsStringList(getArg(values, defaultVal));
}

k8api::string_list_t Component::getArgAsStringList(const string &values)
{
    k8api::string_list_t rval;

    enum class State {
        SKIPPING,
        IN_STRING,
        IN_QUOTED_STRING
    };

    auto state = State::SKIPPING;
    string value;

    for (const char ch : values) {
        switch (state) {
            case State::SKIPPING:
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                continue;
            }
            if (ch == '\'') {
                state = State::IN_QUOTED_STRING;
                continue;
            }
            state = State::IN_STRING;
            break;
        case State::IN_STRING:
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                rval.push_back(value);
                value.clear();
                state = State::SKIPPING;
                continue;
            }
        case State::IN_QUOTED_STRING:
            if (ch == '\'') {
                rval.push_back(value);
                value.clear();
                state = State::SKIPPING;
                continue;
            }
        }

        value += ch;
    }

    if (!value.empty()) {
        rval.push_back(value);
    }

    return rval;
}

k8api::env_vars_t Component::getArgAsEnvList(const string &values, const string &defaultVal) const
{
    return getArgAsEnvList(getArg(values, defaultVal));
}

k8api::env_vars_t Component::getArgAsEnvList(const string &values)
{
    k8api::env_vars_t rval;
    auto list = getArgAsStringList(values);

    for(const auto& v : list) {
        k8api::KeyValue ev;
        if (auto pos = v.find('='); pos != string::npos && pos < v.size()) {
            ev.name = v.substr(0, pos);
            ev.value = v.substr(pos +1);
        } else {
            ev.name = v; // Just an empty variable
        }

        if (!ev.name.empty()) {
            rval.push_back(ev);
        }
    }

    return rval;
}

k8api::key_values_t Component::getArgAsKv(const string &values)
{
    k8api::key_values_t rval;
    auto list = getArgAsStringList(values);

    for(const auto& v : list) {
        if (auto pos = v.find('='); pos != string::npos && pos < v.size()) {
            if (pos > 0) {
                rval[v.substr(0, pos)] = v.substr(pos +1);
            }
        } else {
            if (!v.empty()) {
                rval[v] = "";
            }
        }
    }

    return rval;
}

string Component::getArg(const string &name, const string &defaultVal) const
{
    auto v = getArg(name);
    if (v) {
        return *v;
    }

    return defaultVal;
}

int Component::getIntArg(const string &name, int defaultVal) const
{
    auto v = getArg(name);
    if (v && !v.value().empty()) {
        return stoi(*v);
    }

    return defaultVal;
}

size_t Component::getSizetArg(const string &name, size_t defaultVal) const
{
    auto v = getArg(name);
    if (v) {
        return stoll(*v);
    }

    return defaultVal;
}

Component::ptr_t Component::populateTree(const ComponentDataDef &def, Cluster &cluster)
{
    if (auto root = populate(def, cluster, {})) {
        root->init();
        return root;
    }

    return {};
}

void Component::prepare()
{
    tasks_ = make_unique<tasks_t>();
    switch(Engine::mode()) {
    case Engine::Mode::DEPLOY:
    case Engine::Mode::SHOW_DEPENDENCIES:
        prepareDeploy();
        addDeploymentTasks(*tasks_);
        prepareTasks(*tasks_, false);
        scanDependencies();
        break;
    case Engine::Mode::DELETE:
        prepareDeploy();
        addRemovementTasks(*tasks_);
        prepareTasks(*tasks_, true);
        scanDependencies();
        break;
    }
}

void Component::prepareDeploy()
{
    for(auto& child : children_) {
        child->prepareDeploy();
    }
}

std::future<void> Component::deploy()
{
    assert(isRoot());
    return execute();
}

std::future<void> Component::dumpDependencies()
{
    const auto dotName = name + "-" + Engine::config().dotfile;
    std::ofstream out{dotName};

    if (out.is_open()) {
        LOG_INFO << "Dumping dependencies to: " << dotName;

        out << "digraph {" << endl;
        out << "   subgraph components {" << endl;
        out << R"(      label="Components";)" << endl;

        forAllComponents([&](Component& c) {
            for (const auto& dep : c.dependsOn_) {
                if (auto d = dep.lock()) {
                    out << "      \"" << boost::trim_right_copy(c.logName())
                        << "\" -> \"" << boost::trim_right_copy(d->logName()) << '"' << endl;
                }
            }
        });

        out << "   }" << endl;

        if (tasks_) {
            out << "   subgraph tasks {" << endl;
            out << R"(      label="Tasks";)" << endl;

            for(const auto& t : *tasks_) {
                for (const auto& dw: t->dependencies()) {
                    if (auto d = dw.lock()) {
                        out << "      \"" << boost::trim_right_copy(t->component().logName()) << '.' << t->name()
                            << "\" -> \""
                            << boost::trim_right_copy(d->component().logName()) << '.' << d->name()
                            << '"' << endl;
                    }
                }
            }

            out << "   }" << endl;
        }

        out << "}" << endl;
    }

    return dummyReturnFuture();
}

std::future<void> Component::remove()
{
//    tasks_ = make_unique<tasks_t>();
//    addRemovementTasks(*tasks_);
//    prepareTasks(*tasks_, true);
    return execute();
}

std::future<void> Component::execute()
{
    // Execute via asio's executor
    cluster().client().GetIoService().post([self = weak_from_this()] {
       if (auto component = self.lock()) {
           component->runTasks();
       }
    });

    executionPromise_ = make_unique<promise<void>>();
    return executionPromise_->get_future();
}

void Component::onEvent(const std::shared_ptr<k8api::Event>& event)
{
    if (tasks_) {
        cluster_->client().GetIoService().post([event, self = weak_from_this()] {
            if (auto component = self.lock()) {
                component->processEvent(*event);
            }
        });
    }
}

labels_t::value_type Component::getSelector()
{
    if (auto selector = labels.find("app"); selector != labels.end()) {
        return *selector;
    }

    return {"app", name};
}

void Component::scheduleRunTasks()
{
    if (cluster_->state() != Cluster::State::EXECUTING) {
        LOG_TRACE << logName() << "Skipping schedule. Cluster is is state " << static_cast<int>(cluster_->state());
        return;
    }

    evaluate();
    schedule([wself = getRoot().weak_from_this()] {
       if (auto self = wself.lock()) {
           self->runTasks();
       }
    });
}

void Component::schedule(std::function<void ()> fn)
{
    cluster().client().GetIoService().post([wself = weak_from_this(), fn=std::move(fn)] {
       if (auto self = wself.lock()) {
           if (fn) {
               try {
                fn();
               } catch(const std::exception& ex) {
                   LOG_ERROR << self->logName()
                             <<  "Caught exception from schedule: " << ex.what();
               }
           }
       }
    });
}

bool Component::isBlockedOnDependency() const
{
    if (mode_ == Mode::CREATE) {
        for(const auto& wcomp : dependsOn_) {
            if (auto comp = wcomp.lock()) {
                if (comp->state_ != State::DONE) {
                    LOG_TRACE << logName() << " is still blocked on " << comp->logName();
                    return true;
                }
            }
        }
    }

    return false;
}

// Assumes that all components are created, including generated ones
void Component::scanDependencies()
{
    const auto reverse = mode_ == Mode::REMOVE;
    if (isRoot()) {

        // If we have components for namespaces, make all
        // components using these namespace depend on them.
        const auto ns = getNamespace();
        std::map<std::string, Component *> nsComponents;
        forAllComponents([&](Component& c) {
            if (c.kind_ == Kind::NAMESPACE) {
                nsComponents[c.namespace_.metadata.name] = &c;
            }
        });

        if (!nsComponents.empty()) {
            forAllComponents([&](Component& c) {
                if (const auto ns = c.getNamespace(); !ns.empty()) {
                    if (auto it = nsComponents.find(c.getNamespace()); it != nsComponents.end()) {
                        if (reverse)
                            it->second->addDependency(c);
                        else
                            c.addDependency(*it->second);
                    }
                }
            });
        }
    }

    //if (mode_ == Mode::CREATE) {
        for (const auto& depName : depends) {
            forAllComponents([&](Component& c) {
                if (depName == c.name) {
                    if (reverse)
                        c.addDependency(*this);
                    else
                        addDependency(c);
                }
            });
      //  }

//        for(const auto& child: children_) {
//            if (child->parentRelation() == ParentRelation::BEFORE) {
//                addDependency(*child);
//            } else if (child->parentRelation() == ParentRelation::AFTER) {
//                child->addDependency(*this);
//            }

//            child->scanDependencies();
//        }
    }

    for(const auto& child: children_) {
        child->scanDependencies();
    }

}

Component &Component::getRoot()
{
    auto root = this;
    while (true) {
        if (auto p = root->parent_.lock()) {
            root = p.get();
            continue;
        }
        break;
    }

    return *root;
}

void Component::evaluate()
{
    State newState = State::CREATING;

    if (auto& t = getRoot().tasks_) {
        bool allDone = true;
        size_t numTasks = 0;
        for(const auto& task : *t) {

            // Filter on tasks for this component
            if (&task->component() != this) {
                continue;
            }

            ++numTasks;

            if (task->state() >= Task::TaskState::BLOCKED && state_ == State::CREATING) {
                newState = State::RUNNING;
            }

            if (task->state() != Task::TaskState::DONE) {
                allDone = false;
                LOG_TRACE << logName() << "Blocked on task "
                          << task->component().logName() << task->name()
                          << " in state " << static_cast<size_t>(task->state());
            }

            if (task->state() > Task::TaskState::DONE) {
                // Some varianty of failed
                if (state_ < State::FAILED) {
                    setState(State::FAILED);
                    break;
                }
            }
        }

        if (allDone) {
            bool blockedOnChild = false;
            for(const auto& child: children_) {
                if (child->state_ != State::DONE) {
                    if (child->state_ > State::DONE) {
                        LOG_DEBUG << logName() << "Failed because of " << child->logName();
                        setState(State::FAILED);
                        return;
                    }

                    LOG_TRACE << logName()
                              << "My tasks are all done, but I am still blocked on "
                              << child->logName();

                    blockedOnChild = true;
                }
            }

            if (isBlockedOnDependency()) {
                return;
            }

            if (!blockedOnChild) {
                setState(State::DONE);
                return;
            }
        }

        if (numTasks && newState > state_) {
            setState(newState);
        }
    }
}

string Component::getNamespace() const
{
    if (auto ns = cluster_->getVar("namespace")) {
        return *ns;
    }

    if (auto p = parent_.lock()) {
        return p->getNamespace();
    }

    return Engine::config().ns;
}

void Component::startElapsedTimer()
{
    if (!startTime) {
      startTime = chrono::steady_clock::now();
    }
}

void Component::addDependenciesRecursively(std::set<Component *> &contains)
{
    for(auto& w: dependsOn_) {
        if (auto c = w.lock()) {
            if (contains.insert(c.get()).second) {
                c->addDependenciesRecursively(contains);
            }
        }
    }
}

void Component::processEvent(const k8api::Event& event)
{
    assert(tasks_);
    bool changed = false;
    for(auto& task : *tasks_) {
        changed = task->onEvent(event) ? true : changed;
        if (changed) {
            LOG_TRACE << logName() << " Task " << task->name() << " changed state. Will schedule a re-run of the tasks.";
        }
    }

    if (changed) {
        cluster_->client().GetIoService().post([event, self = weak_from_this()] {
            if (auto component = self.lock()) {
                component->runTasks();
            }
        });
    }
}

void Component::runTasks() {
    if (!tasks_ || cluster_->state() != Cluster::State::EXECUTING) {
        LOG_TRACE << logName() << "Skipping runTasks. Cluster is is state " << static_cast<int>(cluster_->state());
        return;
    }

    // Re-run the loop as long as any task changes it's state
    while(cluster_->isExecuting() && ! isDone()) {
        LOG_TRACE << logName() << "runTasks: Iterating over tasks";
        bool again = false;
        bool allDone = true;
        assert(tasks_);
        for(auto task : *tasks_) {
            again = task->evaluate() ? true : again;
            if (task->state() == Task::TaskState::READY) {
                task->execute();
                again = true;
            }

            allDone = task->isDone() ? allDone : false;
        }

        if (!again) {
            // TODO: Add timer so we can time out if we don't catch or get
            // events to move the states to DONE.

            LOG_TRACE << logName() << "runTasks: Finished iterations for now ...";
            return;
        }
    }

    // TODO: Deal with incomplete state if we are not finished

    LOG_TRACE << logName() << "runTasks: Finished iterations. Cluster isExecuting: "
              << (cluster_->isExecuting() ? "true" : "false")
              << ", isDone: " << (isDone() ? "true" : "false");

}

void Component::setState(Component::State state)
{
    if (state == state_) {
        return;
    }

    if (state == State::DONE) {
        calculateElapsed();
        LOG_INFO << logName() << "Done in " << std::fixed << std::setprecision(5) << (elapsed ? *elapsed : 0.0) << " seconds";

        if (executionPromise_) {
            executionPromise_->set_value();
            executionPromise_.reset();
        }
    }

    if (state == State::FAILED) {
        calculateElapsed();
        LOG_WARN << logName() << "Failed after " << std::fixed << std::setprecision(5) << (elapsed ? *elapsed : 0.0) << " seconds";
    }

    state_ = state;

    if (state_ >= State::RUNNING) {
        if (auto parent = parent_.lock()) {
            parent->evaluate();
            scheduleRunTasks();
        }
    }
}

void Component::forAllComponents(const std::function<void (Component &)>& fn)
{
    getRoot().walkAndExecuteFn(fn);
}

void Component::walkAndExecuteFn(const std::function<void (Component &)>& fn)
{
    fn(*this);
    for(auto& ch : children_) {
        ch->walkAndExecuteFn(fn);
    }
}

void Component::addDeploymentTasks(Component::tasks_t& tasks)
{
    setState(State::RUNNING);
    for(auto& child : children_) {
        child->addDeploymentTasks(tasks);
    }
}

void Component::addRemovementTasks(Component::tasks_t &tasks)
{
    setState(State::RUNNING);
    for(auto& child : children_) {
        child->addRemovementTasks(tasks);
    }
}

Component::ptr_t Component::createComponent(const ComponentDataDef &def,
                                            const Component::ptr_t& parent,
                                            Cluster& cluster)
{
    auto kind = toKind(def.kind);
    switch(kind) {
    case Kind::APP:
        return make_shared<AppComponent>(parent, cluster, def);
    case Kind::JOB:
        return make_shared<JobComponent>(parent, cluster, def);
    case Kind::DEPLOYMENT:
        return make_shared<DeploymentComponent>(parent, cluster, def);
    case Kind::STATEFULSET:
        return make_shared<StatefulSetComponent>(parent, cluster, def);
    case Kind::SERVICE:
        return make_shared<ServiceComponent>(parent, cluster, def);
    case Kind::CONFIGMAP:
        return make_shared<ConfigMapComponent>(parent, cluster, def);
    case Kind::SECRET:
        return make_shared<SecretComponent>(parent, cluster, def);
    case Kind::PERSISTENTVOLUME:
        return make_shared<PersistentVolumeComponent>(parent, cluster, def);
    case Kind::INGRESS:
        return make_shared<IngressComponent>(parent, cluster, def);
    case Kind::NAMESPACE:
        return make_shared<NamespaceComponent>(parent, cluster, def);
    case Kind::DAEMONSET:
        return make_shared<DaemonSetComponent>(parent, cluster, def);
    case Kind::ROLE:
        return make_shared<RoleComponent>(parent, cluster, def);
    case Kind::CLUSTERROLE:
        return make_shared<ClusterRoleComponent>(parent, cluster, def);
    case Kind::ROLEBINDING:
        return make_shared<RoleBindingComponent>(parent, cluster, def);
    case Kind::CLUSTERROLEBINDING:
        return make_shared<ClusterRoleBindingComponent>(parent, cluster, def);
    case Kind::SERVICEACCOUNT:
        return make_shared<ServiceAccountComponent>(parent, cluster, def);
    }

    throw runtime_error("Unknown kind");
}

Component::ptr_t Component::populate(const ComponentDataDef &def,
                         Cluster &cluster,
                         const Component::ptr_t &parent)
{
    const static regex excludeFilter{Engine::config().excludeFilter};
    const static regex includeFilter{Engine::config().includeFilter};

    if (regex_match(def.name, excludeFilter) || !regex_match(def.name, includeFilter)) {
        LOG_INFO << cluster.name() << " Excluding filtered component: " << def.name;
        return {};
    }

    auto component = createComponent(def, parent, cluster);
    if (def.parentRelation == "before") {
        component->parentRelation_ = ParentRelation::BEFORE;
    } else if (def.parentRelation == "after") {
        component->parentRelation_ = ParentRelation::AFTER;
    } else if (def.parentRelation == "independent") {
        component->parentRelation_ = ParentRelation::INDEPENDENT;
    }

    for(auto& childDef : def.children) {
        if (auto child = populate(childDef, cluster, component)) {
            component->children_.push_back(child);
        }
    }

    return component;
}

std::future<void> dummyReturnFuture()
{
    std::promise<void> promise;
    auto future = promise.get_future();
    promise.set_value();
    return future;
}

void Component::validate()
{
    // TODO: Implement.
    // Add sanity checks, like; A Deployment owns it's Service, not the other way around...
}

bool Component::hasKindAsChild(Kind kind) const
{
    for(const auto& child : children_) {
        if (child->getKind() == kind) {
            return true;
        }
    }

    return false;
}

Component *Component::getFirstKindAmongChildren(Kind kind)
{
    for(const auto& child : children_) {
        if (child->getKind() == kind) {
            return child.get();
        }
    }

    return {};
}

void Component::initChildren()
{
    for(auto& child: children_) {
        child->init();
    }
}

Component::ptr_t Component::addChild(const string &name, Kind kind, const labels_t &labels,
                                     const conf_t& args,
                                     const string& parentRelation)
{
    ComponentDataDef def;
    def.labels = labels;
    def.name = name;
    def.kind = toString(kind);
    def.args = args;
    def.parentRelation = parentRelation;
    assert(cluster_);
    auto component = createComponent(def, shared_from_this(), *cluster_);
    component->init();
    children_.push_back(component);
    return component;
}

conf_t Component::mergeArgs() const
{
    conf_t merged = args;
    for(const auto node: getPathToRoot()) {
        for(const auto& [k, v]: node->defaultArgs) {
            // Inconsistency...
            // For some values, we merge the strings, for others we only provide defaults.
            if (k == "pod.args" || k == "pod.env") {
                auto& m = merged[k];
                if (!m.empty()) {
                   m += " ";
                }
                m += v;
            } else {
                merged.try_emplace(k, v);
            }
        }
    }

    return merged;
}

std::vector<const Component*> Component::getPathToRoot() const
{
    std::vector<const Component *> path;
    for(auto p = this; p != nullptr; p = p->parentPtr()) {
        path.push_back(p);
    }
    return path;
}

const Component *Component::parentPtr() const
{
    if (auto parent = parent_.lock()) {
        return parent.get();
    }

    return {};
}

void Component::Task::setState(Component::Task::TaskState state, bool scheduleRunTasks)
{
    LOG_TRACE << component().logName() << " Task " << name() << " change state from "
              << toString(state_) << " to " << toString(state);

    const bool changed = state_ != state;
    state_ = state;

    if (changed && state == TaskState::EXECUTING) {
      component().startElapsedTimer();
    }

    if (state == TaskState::DONE) {
        LOG_DEBUG << component().logName() << "task " << name() << " is done";
    }

    if (changed && scheduleRunTasks) {
        component().scheduleRunTasks();
    }
}

bool Component::Task::evaluate()
{
    bool changed = false;

    if (state_ == TaskState::PRE) {
        changed = true;
        state_ = TaskState::BLOCKED;
    }

    if (state_ == TaskState::BLOCKED) {
        // If any components our component depends on are not done, we are still blocked
        if (mode() == Mode::CREATE) {
            component().evaluate();
            if (component().isBlockedOnDependency()) {
                return changed;
            }
        }

        bool blocked = false;

        for(const auto& dep : dependencies_) {
            // If any dependencies is not DONE, we are still blocked.
            if (auto dt = dep.lock()) {
                blocked = dt->state() == TaskState::DONE ? blocked : true;

                if (dt->state() != TaskState::DONE) {
                    blocked = true;
                    LOG_TRACE << component().logName()
                              << "task " << name()
                              << " is blocked on task "
                              << dt->component().logName()
                              << '/' << dt->name();
                }

                if (dt->state() >= TaskState::ABORTED) {
                    // If a dependency failed, we abort.
                    // TODO: Deal with roll-backs
                    setState(TaskState::DEPENDENCY_FAILED, false);
                    return true;
                }
            }
        }

        if (!blocked) {
            setState(TaskState::READY, false);
            component().evaluate();
            changed = true;
        }
    } // if BLOCKED

    return changed;
}

void Component::Task::schedulePoll()
{
    component().schedule([wself = weak_from_this()] {
        if (auto self = wself.lock()) {
            if (!self->pollTimer_) {
                self->pollTimer_ = make_unique<boost::asio::deadline_timer>(
                            self->component().cluster().client().GetIoService(),
                            boost::posix_time::seconds{2});
                self->pollTimer_->async_wait([wself](auto err) {
                    if (auto self = wself.lock()) {
                        self->pollTimer_.reset();
                        if (err) {
                            LOG_WARN << self->component().logName()
                                     << "Got error from timer for task: " << err;
                            return;
                        }

                        if (!self->component().probe([wself](auto state) {
                            if (auto self = wself.lock()) {
                                if (self->mode() == Mode::REMOVE) {
                                   if (state == K8ObjectState::DONT_EXIST || state == K8ObjectState::DONE) {
                                       self->setState(TaskState::DONE);
                                       self->component().scheduleRunTasks();
                                       return;
                                   }
                                   if (state == K8ObjectState::FAILED) {
                                        self->setState(TaskState::FAILED);
                                        self->component().scheduleRunTasks();
                                        return;
                                   }
                                   self->schedulePoll();
                                   return;
                                }
                                switch(state) {
                                    case K8ObjectState::FAILED:
                                        self->setState(TaskState::FAILED);
                                        self->component().scheduleRunTasks();
                                        break;
                                    case K8ObjectState::DONT_EXIST:
                                    case K8ObjectState::INIT:
                                        self->schedulePoll();
                                        break;
                                    case K8ObjectState::READY:
                                    case K8ObjectState::DONE:
                                        self->setState(TaskState::DONE);
                                        self->component().scheduleRunTasks();
                                }
                            }
                        })) {
                            // Probes unavailable
                            LOG_DEBUG << self->component().logName() << "Probes not available";
                        }
                    }
                });
            }
        }
    });
}

const string &Component::Task::toString(const Component::Task::TaskState &state) {
    static const array<string, 9> names = { "PRE",
                                            "BLOCKED",
                                            "READY",
                                            "EXECUTING",
                                            "WAITING", // Waiting for events to update it's status
                                            "DONE",
                                            "ABORTED",
                                            "FAILED",
                                            "DEPENDENCY_FAILED"};

    return names.at(static_cast<size_t>(state));
}

void Component::Task::addDependency(const Component::Task::wptr_t &task) {
    if (auto t = task.lock()) {
        for(auto wd: dependencies_) {
            if (wd.lock() == t) {
                return; // Already there
            }
        }
        dependencies_.push_back(task);
    }
}

void Component::Task::addAllDependencies(std::set<Component::Task *> &tasks)
{
    for(auto& d : dependencies_) {
        if (auto dep = d.lock()) {
            tasks.insert(dep.get());
            dep->addAllDependencies(tasks);
        }
    }
}

const JsonFieldMapping *jsonFieldMappings()
{
    static const JsonFieldMapping mappings = {
        {"namespace_", "namespace"},
        {"template_", "template"},
        {"operator_", "operator"},
        {"continue_", "continue"}
    };

    return &mappings;
}

string fileToJson(const string &pathToFile, bool assumeYaml)
{
    if (!filesystem::is_regular_file(pathToFile)) {
        LOG_ERROR << "Not a file: " << pathToFile;
        throw runtime_error("Not a file: "s + pathToFile);
    }

    string json;
    const filesystem::path path{pathToFile};
    const auto ext = path.extension();
    if (assumeYaml || ext == ".yaml") {
        // https://www.commandlinefu.com/commands/view/12218/convert-yaml-to-json
        const auto expr = R"(import sys, yaml, json; json.dump(yaml.load(open(")"s
                + pathToFile
                + R"(","r").read()), sys.stdout, indent=4))"s;
        auto args = boost::process::args({"-c"s, expr});
        boost::process::ipstream pipe_stream;
        boost::process::child process(boost::process::search_path("python"),
                                      args,
                                      boost::process::std_out > pipe_stream);
        string line;
        while (pipe_stream && std::getline(pipe_stream, line)) {
            json += line;
        }

        error_code ec;
        process.wait(ec);
        if (ec) {
            LOG_ERROR << "Failed to convert yaml from " << pathToFile << ": " << ec.message();
            throw runtime_error("Failed to convert yaml: "s + pathToFile);
        }

    } else if (ext == ".json") {
        json = slurp(pathToFile);
    } else {
        LOG_ERROR << "File extension must be yaml or json: " << pathToFile;
        throw runtime_error("Unknown extension "s + pathToFile);
    }

    return json;
}

void Component::sendDelete(const string &url, std::weak_ptr<Component::Task> task,
                           bool ignoreErrors,
                           const initializer_list<std::pair<string, string>>& args)
{
    client().Process([this, url, task, ignoreErrors, args](auto& ctx) {

        LOG_DEBUG << logName() << "Sending DELETE " << url;

        try {
            auto reply = restc_cpp::RequestBuilder{ctx}.Req(url, Request::Type::DELETE, args)
               .Execute();

            LOG_DEBUG << logName()
                  << "Delete gave response: "
                  << reply->GetResponseCode() << ' '
                  << reply->GetHttpResponse().reason_phrase;

            // We don't get any event's related to deleting the deployment, so just update the states.
            if (auto taskInstance = task.lock()) {
                taskInstance->setState(Task::TaskState::DONE);
            }
            return;
        } catch(const restc_cpp::RequestFailedWithErrorException& err) {
            if (err.http_response.status_code == 404) {
                // Perfectly OK
                if (auto taskInstance = task.lock()) {
                    LOG_DEBUG << logName()
                             << "Ignoring failed DELETE request: " << err.http_response.status_code
                             << ' ' << err.http_response.reason_phrase
                             << ": \"" << err.what()
                             << "\" for url: " << url;

                    taskInstance->setState(Task::TaskState::DONE);
                }
                return;
            }

            LOG_WARN << logName()
                     << "Request failed: " << err.http_response.status_code
                     << ' ' << err.http_response.reason_phrase
                     << ": " << err.what();

        } catch(const std::exception& ex) {
            LOG_WARN << logName()
                     << "Request failed: " << ex.what();
        }

        if (auto taskInstance = task.lock()) {
            taskInstance->setState(ignoreErrors ? Task::TaskState::DONE : Task::TaskState::FAILED);
        }

        if (!ignoreErrors) {
            setState(State::FAILED);
        }
    });
}

void Component::calculateElapsed()
{
    if (startTime) {
        const auto ended = chrono::steady_clock::now();
        elapsed = chrono::duration<double>(ended - *startTime).count();
    }
}

void Component::addDependency(Component &component)
{
    if (&component == this) {
        LOG_ERROR << logName() << "Cannot add dependency to myslelf!";
        throw runtime_error("Cannot depend on myself!");
    }

    std::set<Component *> deps;
    component.addDependenciesRecursively(deps);
    if (deps.find(this) != deps.end()) {
        LOG_ERROR << logName() << "Detected circular dependency with: " << component.logName();
        throw runtime_error("Circular dependency");
    }

    for(const auto& w : dependsOn_) {
        if (const auto c = w.lock()) {
            if (c.get() == &component) {
                return;
            }
        }
    }

    LOG_DEBUG << logName() << "Component depends on " << component.logName();
    dependsOn_.push_back(component.weak_from_this());
}

void Component::prepareTasks(tasks_t& tasks, bool reverseDependencies)
{
//    // Built list of tasks
//    tasks_ = make_unique<tasks_t>();
//    fn(*tasks_);

    const bool isDelete = Engine::mode() == Engine::Mode::DELETE;

    // Set dependencies
    if (!isDelete) {
        for(auto& task : tasks) {
            auto relation = task->component().parentRelation();
            if (reverseDependencies) {
                if (relation == ParentRelation::AFTER) {
                    relation = ParentRelation::BEFORE;
                } else if (relation == ParentRelation::BEFORE) {
                    relation = ParentRelation::AFTER;
                }
            }

            switch(relation) {
            case ParentRelation::AFTER:
                // The task depend on parent task(s)
                if (auto parent = task->component().parent_.lock()) {
                    for(auto ptask : tasks) {
                        if (&ptask->component() == parent.get()) {
                            LOG_TRACE << task->component().logName() << "Task " << task->name() << " depends on " << ptask->name();
                            task->addDependency(ptask->weak_from_this());
                        }
                    }
                }
                break;
            case ParentRelation::BEFORE:
                // The parent's tasks depend on the task(s)
                if (auto parent = task->component().parent_.lock()) {
                    for(auto ptask : tasks) {
                        if (&ptask->component() == parent.get()) {
                            LOG_TRACE << task->component().logName() << "Task " << ptask->name() << " depends on " << task->name();
                            ptask->addDependency(task->weak_from_this());
                        }
                    }
                }
                break;
            case ParentRelation::INDEPENDENT:
                ; // Don't matter
            }
        }
    }

    // Check for circular dependencies
    for (const auto& task : tasks) {
        std::set<Task *> allDeps;
        task->addAllDependencies(allDeps);
        if (auto it = allDeps.find(task.get()) ; it != allDeps.end()) {
            LOG_ERROR << task->component().logName() << "task " << task->name() << " Circular dependency to "
            << (*it)->component().logName() << (*it)->name();
            throw runtime_error("Circular dependency");
        }
    }
}

string getVar(const std::string& name, const variables_t& vars,
              const optional<string>& defaultValue) {
    if (auto it = vars.find(name); it != vars.end()) {
        return it->second;
    }

    if (auto val = getenv(name.c_str())) {
        return val;
    }

    if (defaultValue) {
        return *defaultValue;
    }

    return {};
}

string expandVariables(const string &json, const variables_t &vars)
{
    // Var: ${varname[,default value]}
    // Default is unset/null

    enum class State {
        COPY,
        BACKSLASH,
        DOLLAR,
        SCAN_NAME,
        SCAN_DEFAUT_VALUE,
    };

    locale loc{"C"};
    string expanded;
    expanded.reserve(json.size());
    auto state = State::COPY;
    string varName;
    optional<string> defaultValue;
    for(auto ch : json) {
        switch(state) {
        case State::COPY:
            if (ch == '\\') {
                state = State::BACKSLASH;
                break;
            }
            if (ch == '$') {
                state = State::DOLLAR;
                break;
            }
            expanded += ch;
            break;
        case State::BACKSLASH:
            if (ch != '$') {
                expanded += '\\';
            }
            expanded += ch;
            state = State::COPY;
            break;
        case State::DOLLAR:
            if (ch == '{') {
                state = State::SCAN_NAME;
                varName.clear();
                defaultValue.reset();
                break;
            }
            expanded += '$';
            expanded += ch;
            state = State::COPY;
            break;
        case State::SCAN_NAME:
            if (isalnum(ch, loc) || ch == '.' || ch == '_') {
                varName += ch;
                break;
            }
            if (ch == ',') {
                defaultValue.emplace();
                state = State::SCAN_DEFAUT_VALUE;
                break;
            }
commit:
            if (ch == '}') {
                // Commint variable
                if (defaultValue && !defaultValue->empty() && defaultValue->at(0) == '$') {
                    if (const auto evar = getenv(defaultValue->substr(1).c_str())) {
                        *defaultValue = *evar;
                    }
                }
                expanded += getVar(varName, vars, defaultValue);
                state = State::COPY;
                break;
            }

            LOG_ERROR << "Error scanning variable-name starting with: " << varName;
            throw runtime_error("Error expanding macro");

        case State::SCAN_DEFAUT_VALUE:
            if (ch == '}') {
                goto commit;
            }

            if (ch == '"') {
                *defaultValue  += '\\';
            }
            *defaultValue += ch;
        }
    }

    if (state != State::COPY) {
        LOG_ERROR << "Error expanding macro " << varName << ": Not properly terminated with '}'";
        throw runtime_error("Error expanding macro");
    }

    return expanded;
}


} // ns
