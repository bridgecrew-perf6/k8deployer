
#include "restc-cpp/RequestBuilder.h"

#include "k8deployer/logging.h"
#include "k8deployer/DeploymentComponent.h"
#include "k8deployer/Cluster.h"
#include "k8deployer/Engine.h"

using namespace std;
using namespace string_literals;
using namespace restc_cpp;

namespace k8deployer {

std::future<void> DeploymentComponent::prepareDeploy()
{
    if (deployment.metadata.name.empty()) {
        deployment.metadata.name = name;
    }

    if (deployment.metadata.namespace_.empty()) {
        deployment.metadata.namespace_ = Engine::config().ns;
    }

    auto selector = getSelector();

    deployment.metadata.labels.try_emplace(selector.first, selector.second);
    deployment.spec.selector.matchLabels.try_emplace(selector.first, selector.second);

    if (deployment.spec.template_.metadata.name.empty()) {
        deployment.spec.template_.metadata.name = name;
    }
    deployment.spec.template_.metadata.labels.try_emplace(selector.first, selector.second);

    if (auto replicas = getArg("replicas")) {
        deployment.spec.replicas = stoull(*replicas);
    }

    deployment.spec.selector.matchLabels.try_emplace(selector.first, selector.second);
    deployment.spec.template_.metadata.labels.try_emplace(selector.first, selector.second);

    if (deployment.spec.template_.spec.containers.empty()) {

        k8api::Container container;
        container.name = name;
        container.image = getArg("image", name);

        if (auto port = getArg("port")) {
            k8api::ContainerPort p;
            p.containerPort = static_cast<uint16_t>(stoul(*port));
            p.name = "default";
            if (auto v = getArg("protocol")) {
                p.protocol = *v;
            }
            container.ports.emplace_back(p);
        }

        deployment.spec.template_.spec.containers.push_back(move(container));
    }

    buildDependencies();

    // Apply recursively
    Component::prepareDeploy();

    return dummyReturnFuture();
}

void DeploymentComponent::addTasks(Component::tasks_t& tasks)
{
    auto task = make_shared<Task>(*this, name, [&](Task& task, const k8api::Event *event) {

        // Execution?
        if (task.state() == Task::TaskState::READY) {
            task.setState(Task::TaskState::EXECUTING);
            doDeploy(task.weak_from_this());
            task.setState(Task::TaskState::WAITING);
        }

        // Monitoring?
        if (task.isMonitoring() && event) {
            LOG_TRACE << task.component().logName() << " Task " << task.name()
                      << " evaluating event: " << event->message;

            auto key = name + "-";
            if (event->involvedObject.kind == "Pod"
                && event->involvedObject.name.substr(0, key.size()) == key
                && event->metadata.namespace_ == deployment.metadata.namespace_
                && event->metadata.name.substr(0, key.size()) == key
                && event->reason == "Created") {

                // A pod with our name was created.
                // TODO: Use this as a hint and query the status of running pods.
                // In order to continue, all our pods should be in avaiable state

                LOG_DEBUG << "A pod with my name was created. I am assuming it relates to me";
                if (++podsStarted_ >= deployment.spec.replicas) {
                    task.setState(Task::TaskState::DONE);
                    setState(State::DONE);
                }
            }
        }

        task.evaluate();
    });

    tasks.push_back(task);

    Component::addTasks(tasks);
}


void DeploymentComponent::buildDependencies()
{
    if (labels.empty()) {
        labels["app"] = name; // Use the name as selector
    }

    // A deployment normally needs a service
    const auto serviceEnabled = getBoolArg("service.enabled");
    if (!hasKindAsChild(Kind::SERVICE) && (serviceEnabled && *serviceEnabled)) {
        LOG_DEBUG << logName() << "Adding Service.";

        conf_t svcargs;
        // Since we create the service, give it a copy of relevant arguments..
        for(const auto [k, v] : args) {
            static const array<string, 2> relevant = {"service.nodePort", "service.type"};
            if (find(relevant.begin(), relevant.end(), k) != relevant.end()) {
                svcargs[k] = v;
            }
        }

        addChild(name + "-svc", Kind::SERVICE, labels, svcargs);
    }

    // Check for config-files --> ConfigMap
    if (auto fileNames = getArg("config.fromFile")) {
        LOG_DEBUG << logName() << "Adding ConfigMap.";

        conf_t svcargs;
        for(const auto [k, v] : args) {
            static const array<string, 1> relevant = {"config.fromFile"};
            if (find(relevant.begin(), relevant.end(), k) != relevant.end()) {
                svcargs[k] = v;
            }
        }

        auto cf = addChild(name + "-conf", Kind::CONFIGMAP, {}, svcargs);
        cf->prepareDeploy(); // We need the fully initialized ConfigMap in order to map the volume

        // Add the configmap as a volume to the first pod
        k8api::PodSpec *podspec = {};
        podspec = &deployment.spec.template_.spec;

        k8api::Volume volume;
        volume.name = cf->configmap.metadata.name;
        volume.configMap.name = cf->configmap.metadata.name;

        for(auto& [k, _] : cf->configmap.binaryData) {
            k8api::KeyToPath ktp;
            ktp.key = k;
            ktp.path = k;
            ktp.mode = 0440;
            volume.configMap.items.push_back(ktp);
        }

        podspec->volumes.push_back(volume);

        // Now, add the mount point to the containers so they can use it
        // Assume `/config` for auto-added volumes from "config.fromFile"
        k8api::VolumeMount vm;
        vm.name = volume.name;
        vm.mountPath = "/config";
        vm.readOnly = true;

        for(auto& container: podspec->containers) {
            container.volumeMounts.push_back(vm);
        }
    }
}

void DeploymentComponent::doDeploy(std::weak_ptr<Task> task)
{
    auto url = cluster_->getUrl()
            + "/apis/apps/v1/namespaces/"
            + deployment.metadata.namespace_
            + "/deployments";

    Engine::client().Process([this, url, task](Context& ctx) {

        LOG_DEBUG << logName()
                  << "Sending Deployment "
                  << deployment.metadata.name;

        LOG_TRACE << "Payload: " << toJson(deployment);

        try {
            auto reply = RequestBuilder{ctx}.Post(url)
               .Data(deployment, jsonFieldMappings())
               .Execute();

            LOG_DEBUG << logName()
                  << "Applying gave response: "
                  << reply->GetResponseCode() << ' '
                  << reply->GetHttpResponse().reason_phrase;
            return;
        } catch(const RequestFailedWithErrorException& err) {
            LOG_WARN << logName()
                     << "Request failed: " << err.http_response.status_code
                     << ' ' << err.http_response.reason_phrase
                     << ": " << err.what();

        } catch(const std::exception& ex) {
            LOG_WARN << logName()
                     << "Request failed: " << ex.what();
        }

        if (auto taskInstance = task.lock()) {
            taskInstance->setState(Task::TaskState::FAILED);
        }
        setState(State::FAILED);

    });
}


}
