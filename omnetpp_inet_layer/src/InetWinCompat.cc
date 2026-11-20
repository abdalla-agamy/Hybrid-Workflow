#if defined(_WIN32) && !defined(OMNETPP_STANDALONE_TEST)

#include "inet/common/ModuleMixin.h"
#include "inet/common/StringFormat.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace inet {
namespace {

class cCompatCollectObjectsVisitor : public cVisitor {
public:
    const char* name;
    std::vector<cObject*> objects;

    explicit cCompatCollectObjectsVisitor(const char* name) : name(name) {}

protected:
    virtual bool visit(cObject* object) override {
        if (object->isName(name)) {
            objects.push_back(object);
        }
        return true;
    }
};

} // namespace

namespace internal {

// INET 4.7.0 omits INET_API on these two helpers declared in ModuleMixin.h, so
// downstream Windows DLL/EXE targets linking against libINET.dll.a must supply them.
void refreshDisplayString(cModule* thisModule, const StringFormat::IResolver* thisModuleAsResolver) {
    if (thisModule->hasPar("displayStringTextFormat")) {
        try {
            auto displayStringTextFormat = thisModule->par("displayStringTextFormat").stringValue();
            if (!opp_isempty(displayStringTextFormat)) {
                auto text = StringFormat::formatString(displayStringTextFormat, thisModuleAsResolver);
                thisModule->getDisplayString().setTagArg("t", 0, text.c_str());
            }
        } catch (cException& e) {
            e.prependMessage("While processing displayStringTextFormat: ");
            throw;
        }
    }
}

std::string doResolveExpression(cModule* thisModule, const char* expression) {
    const char* lastDot = std::strrchr(expression, '.');
    cModule* targetModule = thisModule;
    const char* fieldName = expression;

    if (lastDot != nullptr) {
        std::string submodulePath(expression, lastDot - expression);
        targetModule = thisModule->getModuleByPath(submodulePath.c_str());
        fieldName = lastDot + 1;
    }

    cCompatCollectObjectsVisitor visitor(fieldName);
    visitor.processChildrenOf(targetModule);

    if (visitor.objects.empty()) {
        throw cRuntimeError("Unknown expression: %s", expression);
    }

    if (visitor.objects.size() > 1) {
        std::stable_sort(
            visitor.objects.begin(),
            visitor.objects.end(),
            [](const cObject* o1, const cObject* o2) {
                return dynamic_cast<const cWatchBase*>(o1) != nullptr &&
                       dynamic_cast<const cWatchBase*>(o2) == nullptr;
            }
        );
    }

    if (auto* par = dynamic_cast<cPar*>(visitor.objects[0])) {
        if (par->getType() == cPar::STRING) {
            return par->stdstringValue();
        }
    }
    if (auto* watchBase = dynamic_cast<cWatchBase*>(visitor.objects[0])) {
        any_ptr ptr = watchBase->getValuePointer();
        if (ptr.contains<std::string>()) {
            return *ptr.get<std::string>();
        }
    }

    return visitor.objects[0]->str();
}

} // namespace internal
} // namespace inet

#endif // _WIN32 && !OMNETPP_STANDALONE_TEST
