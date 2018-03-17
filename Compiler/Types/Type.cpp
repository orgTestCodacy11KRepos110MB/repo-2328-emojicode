//
//  Type.c
//  Emojicode
//
//  Created by Theo Weidmann on 04.03.15.
//  Copyright (c) 2015 Theo Weidmann. All rights reserved.
//

#include "Type.hpp"
#include "Class.hpp"
#include "CommonTypeFinder.hpp"
#include "Emojis.h"
#include "Enum.hpp"
#include "Extension.hpp"
#include "Functions/Function.hpp"
#include "Package/Package.hpp"
#include "Protocol.hpp"
#include "TypeContext.hpp"
#include "Utils/StringUtils.hpp"
#include "ValueType.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace EmojicodeCompiler {

const std::u32string kDefaultNamespace = std::u32string(1, E_HOUSE_BUILDING);

Type::Type(Protocol *protocol)
    : typeContent_(TypeType::Protocol), typeDefinition_(protocol) {}

Type::Type(Enum *enumeration)
    : typeContent_(TypeType::Enum), typeDefinition_(enumeration) {}

Type::Type(Extension *extension)
    : typeContent_(TypeType::Extension), typeDefinition_(extension) {}

Type::Type(ValueType *valueType)
: typeContent_(TypeType::ValueType), typeDefinition_(valueType), mutable_(false) {
    for (size_t i = 0; i < valueType->genericParameters().size(); i++) {
        genericArguments_.emplace_back(i, valueType, true);
    }
}

Type::Type(Class *klass) : typeContent_(TypeType::Class), typeDefinition_(klass) {
    for (size_t i = 0; i < klass->genericParameters().size() + klass->superGenericArguments().size(); i++) {
        genericArguments_.emplace_back(i, klass, true);
    }
}

Class* Type::eclass() const {
    return dynamic_cast<Class *>(typeDefinition_);
}

Protocol* Type::protocol() const {
    return dynamic_cast<Protocol *>(typeDefinition_);
}

Enum* Type::eenum() const {
    return dynamic_cast<Enum *>(typeDefinition_);
}

ValueType* Type::valueType() const {
    return dynamic_cast<ValueType *>(typeDefinition_);
}

TypeDefinition* Type::typeDefinition() const  {
    return typeDefinition_;
}

bool Type::canHaveGenericArguments() const {
    return type() == TypeType::Class || type() == TypeType::Protocol || type() == TypeType::ValueType;
}

void Type::sortMultiProtocolType() {
    std::sort(genericArguments_.begin(), genericArguments_.end(), [](const Type &a, const Type &b) {
        return a.protocol() < b.protocol();
    });
}

size_t Type::genericVariableIndex() const {
    if (type() != TypeType::GenericVariable && type() != TypeType::LocalGenericVariable) {
        throw std::domain_error("Tried to get reference from non-reference type");
    }
    return genericArgumentIndex_;
}

Type Type::resolveReferenceToBaseReferenceOnSuperArguments(const TypeContext &typeContext) const {
    TypeDefinition *c = typeContext.calleeType().typeDefinition();
    Type t = *this;

    // Try to resolve on the generic arguments to the superclass.
    while (t.type() == TypeType::GenericVariable && c->canBeUsedToResolve(t.typeDefinition()) &&
           t.genericVariableIndex() < c->superGenericArguments().size()) {
        Type tn = c->superGenericArguments()[t.genericVariableIndex()];
        if (tn.type() == TypeType::GenericVariable && tn.genericVariableIndex() == t.genericVariableIndex()
            && tn.typeDefinition() == t.typeDefinition()) {
            break;
        }
        t = tn;
    }
    return t;
}

Type Type::resolveOnSuperArgumentsAndConstraints(const TypeContext &typeContext) const {
    if (typeContext.calleeType().type() == TypeType::NoReturn) {
        return *this;
    }
    TypeDefinition *c = typeContext.calleeType().typeDefinition();
    Type t = *this;
    if (type() == TypeType::NoReturn) {
        return t;
    }
    if (type() == TypeType::Optional) {
        t.genericArguments_[0] = genericArguments_[0].resolveOnSuperArgumentsAndConstraints(typeContext);
        return t;
    }

    bool box = t.storageType() == StorageType::Box;

    // Try to resolve on the generic arguments to the superclass.
    while (t.type() == TypeType::GenericVariable && t.genericVariableIndex() < c->superGenericArguments().size()) {
        t = c->superGenericArguments()[t.genericArgumentIndex_];
    }
    while (t.type() == TypeType::LocalGenericVariable && typeContext.function() == t.localResolutionConstraint_) {
        t = typeContext.function()->constraintForIndex(t.genericArgumentIndex_);
    }
    while (t.type() == TypeType::GenericVariable
           && typeContext.calleeType().typeDefinition()->canBeUsedToResolve(t.typeDefinition())) {
        t = typeContext.calleeType().typeDefinition()->constraintForIndex(t.genericArgumentIndex_);
    }

    if (box) {
        t.forceBox_ = true;
    }
    return t;
}

Type Type::resolveOn(const TypeContext &typeContext) const {
    Type t = *this;
    if (type() == TypeType::NoReturn) {
        return t;
    }
    if (type() == TypeType::Optional) {
        t.genericArguments_[0] = genericArguments()[0].resolveOn(typeContext);
        return t;
    }

    bool box = t.storageType() == StorageType::Box;

    while (t.type() == TypeType::LocalGenericVariable && typeContext.function() == t.localResolutionConstraint_
           && typeContext.functionGenericArguments() != nullptr) {
        t = (*typeContext.functionGenericArguments())[t.genericVariableIndex()];
    }

    if (typeContext.calleeType().canHaveGenericArguments()) {
        while (t.type() == TypeType::GenericVariable &&
               typeContext.calleeType().typeDefinition()->canBeUsedToResolve(t.typeDefinition())) {
            Type tn = typeContext.calleeType().genericArguments_[t.genericVariableIndex()];
            if (tn.type() == TypeType::GenericVariable && tn.genericVariableIndex() == t.genericVariableIndex()
                && tn.typeDefinition() == t.typeDefinition()) {
                break;
            }
            t = tn;
        }
    }

    for (auto &arg : t.genericArguments_) {
        arg = arg.resolveOn(typeContext);
    }

    if (box) {
        t.forceBox_ = true;
    }
    return t;
}

bool Type::identicalGenericArguments(Type to, const TypeContext &typeContext, std::vector<CommonTypeFinder> *ctargs) const {
    for (size_t i = to.typeDefinition()->superGenericArguments().size(); i < to.genericArguments().size(); i++) {
        if (!this->genericArguments_[i].identicalTo(to.genericArguments_[i], typeContext, ctargs)) {
            return false;
        }
    }
    return true;
}

bool Type::compatibleTo(const Type &to, const TypeContext &tc, std::vector<CommonTypeFinder> *ctargs) const {
    if (type() == TypeType::Error) {
        return to.type() == TypeType::Error && genericArguments()[0].identicalTo(to.genericArguments()[0], tc, ctargs)
                && genericArguments()[1].compatibleTo(to.genericArguments()[1], tc);
    }
    if (to.type() == TypeType::Something) {
        return true;
    }

    if (this->type() == TypeType::Optional) {
        if (to.type() != TypeType::Optional) {
            return false;
        }
        return optionalType().compatibleTo(to.optionalType(), tc, ctargs);
    }
    if (this->type() != TypeType::Optional && to.type() == TypeType::Optional) {
        return compatibleTo(to.optionalType(), tc, ctargs);
    }

    if ((this->type() == TypeType::GenericVariable && to.type() == TypeType::GenericVariable) ||
        (this->type() == TypeType::LocalGenericVariable && to.type() == TypeType::LocalGenericVariable)) {
        return (this->genericVariableIndex() == to.genericVariableIndex() &&
                this->typeDefinition() == to.typeDefinition()) ||
        this->resolveOnSuperArgumentsAndConstraints(tc)
        .compatibleTo(to.resolveOnSuperArgumentsAndConstraints(tc), tc, ctargs);
    }
    if (type() == TypeType::GenericVariable) {
        return resolveOnSuperArgumentsAndConstraints(tc).compatibleTo(to, tc, ctargs);
    }
    if (type() == TypeType::LocalGenericVariable) {
        return ctargs != nullptr || resolveOnSuperArgumentsAndConstraints(tc).compatibleTo(to, tc, ctargs);
    }

    switch (to.type()) {
        case TypeType::TypeAsValue:
            return type() == TypeType::TypeAsValue && typeOfTypeValue().compatibleTo(to.typeOfTypeValue(), tc, ctargs);
        case TypeType::GenericVariable:
            return compatibleTo(to.resolveOnSuperArgumentsAndConstraints(tc), tc, ctargs);
        case TypeType::LocalGenericVariable:
            if (ctargs != nullptr) {
                (*ctargs)[to.genericVariableIndex()].addType(*this, tc);
                return true;
            }
            return this->compatibleTo(to.resolveOnSuperArgumentsAndConstraints(tc), tc, ctargs);
        case TypeType::Class:
            return type() == TypeType::Class && eclass()->inheritsFrom(to.eclass()) &&
                identicalGenericArguments(to, tc, ctargs);
        case TypeType::ValueType:
            return type() == TypeType::ValueType && typeDefinition() == to.typeDefinition() &&
                identicalGenericArguments(to, tc, ctargs);
        case TypeType::Enum:
            return type() == TypeType::Enum && eenum() == to.eenum();
        case TypeType::Someobject:
            return type() == TypeType::Class || type() == TypeType::Someobject;
        case TypeType::Error:
            return compatibleTo(to.genericArguments()[1], tc);
        case TypeType::MultiProtocol:
            return isCompatibleToMultiProtocol(to, tc, ctargs);
        case TypeType::Protocol:
            return isCompatibleToProtocol(to, tc, ctargs);
        case TypeType::Callable:
            return isCompatibleToCallable(to, tc, ctargs);
        case TypeType::NoReturn:
            return type() == TypeType::NoReturn;
        default:
            break;
    }
    return false;
}

bool Type::isCompatibleToMultiProtocol(const Type &to, const TypeContext &ct, std::vector<CommonTypeFinder> *ctargs) const {
    if (type() == TypeType::MultiProtocol) {
        return std::equal(protocols().begin(), protocols().end(), to.protocols().begin(), to.protocols().end(),
                          [ct, ctargs](const Type &a, const Type &b) {
                              return a.compatibleTo(b, ct, ctargs);
                          });
    }

    return std::all_of(to.protocols().begin(), to.protocols().end(), [this, ct](const Type &p) {
        return compatibleTo(p, ct);
    });
}

bool Type::isCompatibleToProtocol(const Type &to, const TypeContext &ct, std::vector<CommonTypeFinder> *ctargs) const {
    if (type() == TypeType::Class) {
        for (Class *a = this->eclass(); a != nullptr; a = a->superclass()) {
            for (auto &protocol : a->protocols()) {
                if (protocol.resolveOn(TypeContext(*this)).compatibleTo(to.resolveOn(ct), ct, ctargs)) {
                    return true;
                }
            }
        }
        return false;
    }
    if (type() == TypeType::ValueType || type() == TypeType::Enum) {
        for (auto &protocol : typeDefinition()->protocols()) {
            if (protocol.resolveOn(TypeContext(*this)).compatibleTo(to.resolveOn(ct), ct, ctargs)) {
                return true;
            }
        }
        return false;
    }
    if (type() == TypeType::Protocol) {
        return this->typeDefinition() == to.typeDefinition() && identicalGenericArguments(to, ct, ctargs);
    }
    return false;
}

bool Type::isCompatibleToCallable(const Type &to, const TypeContext &ct, std::vector<CommonTypeFinder> *ctargs) const {
    if (type() == TypeType::Callable) {
        if (genericArguments_[0].compatibleTo(to.genericArguments_[0], ct, ctargs)
            && to.genericArguments_.size() == genericArguments_.size()) {
            for (size_t i = 1; i < to.genericArguments_.size(); i++) {
                if (!to.genericArguments_[i].compatibleTo(genericArguments_[i], ct, ctargs)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool Type::identicalTo(Type to, const TypeContext &tc, std::vector<CommonTypeFinder> *ctargs) const {
    if (ctargs != nullptr && to.type() == TypeType::LocalGenericVariable) {
        (*ctargs)[to.genericVariableIndex()].addType(*this, tc);
        return true;
    }

    if (type() == to.type()) {
        switch (type()) {
            case TypeType::Class:
            case TypeType::Protocol:
            case TypeType::ValueType:
                return typeDefinition() == to.typeDefinition()
                && identicalGenericArguments(to, tc, ctargs);
            case TypeType::Callable:
                return to.genericArguments_.size() == this->genericArguments_.size()
                && identicalGenericArguments(to, tc, ctargs);
            case TypeType::Optional:
            case TypeType::TypeAsValue:
                return genericArguments_[0].identicalTo(to.genericArguments_[0], tc, ctargs);
            case TypeType::Enum:
                return eenum() == to.eenum();
            case TypeType::GenericVariable:
            case TypeType::LocalGenericVariable:
                return resolveReferenceToBaseReferenceOnSuperArguments(tc).genericVariableIndex() ==
                to.resolveReferenceToBaseReferenceOnSuperArguments(tc).genericVariableIndex();
            case TypeType::Something:
            case TypeType::Someobject:
            case TypeType::NoReturn:
                return true;
            case TypeType::Error:
                return genericArguments_[0].identicalTo(to.genericArguments_[0], tc, ctargs) &&
                genericArguments_[1].identicalTo(to.genericArguments_[1], tc, ctargs);
            case TypeType::MultiProtocol:
                return std::equal(protocols().begin(), protocols().end(), to.protocols().begin(), to.protocols().end(),
                                  [&tc, ctargs](const Type &a, const Type &b) { return a.identicalTo(b, tc, ctargs); });
            case TypeType::StorageExpectation:
            case TypeType::Extension:
                return false;
        }
    }
    return false;
}

StorageType Type::storageType() const {
    if (forceBox_ || requiresBox()) {
        return StorageType::Box;
    }
    if (type() == TypeType::Optional || type() == TypeType::Error) {
        return StorageType::SimpleOptional;
    }
    return StorageType::Simple;
}

bool Type::requiresBox() const {
    switch (type()) {
        case TypeType::Error:
            return genericArguments()[1].storageType() == StorageType::Box;
        case TypeType::Optional:
            return optionalType().storageType() == StorageType::Box;
        case TypeType::Something:
        case TypeType::Protocol:
        case TypeType::MultiProtocol:
            return true;
        case TypeType::GenericVariable:
        case TypeType::LocalGenericVariable:
            return forceBox_;
        default:
            return false;
    }
}

bool Type::isReferencable() const {
    switch (type()) {
        case TypeType::Callable:
        case TypeType::Class:
        case TypeType::Someobject:
        case TypeType::GenericVariable:
        case TypeType::LocalGenericVariable:
        case TypeType::TypeAsValue:
            return storageType() != StorageType::Simple;
        case TypeType::NoReturn:
        case TypeType::Optional:  // only reached in case of error, CompilerError will be/was raised
            return false;
        case TypeType::ValueType:
        case TypeType::Enum:
        case TypeType::Protocol:
        case TypeType::MultiProtocol:
        case TypeType::Something:
        case TypeType::Error:
            return true;
        case TypeType::StorageExpectation:
        case TypeType::Extension:
            throw std::logic_error("isReferenceWorthy for StorageExpectation/Extension");
    }
}

std::string Type::typePackage() const {
    switch (this->type()) {
        case TypeType::Class:
        case TypeType::ValueType:
        case TypeType::Protocol:
        case TypeType::Enum:
            return typeDefinition()->package()->name();
        case TypeType::StorageExpectation:
        case TypeType::Extension:
            throw std::logic_error("typePackage for StorageExpectation/Extension");
        default:
            return "";
    }
}

void Type::typeName(Type type, const TypeContext &typeContext, std::string &string, bool package) const {
    if (package) {
        string.append(type.typePackage());
    }

    switch (type.type()) {
        case TypeType::Optional:
            string.append("🍬");
            typeName(type.genericArguments_[0], typeContext, string, package);
            return;
        case TypeType::TypeAsValue:
            string.append("🔳");
            typeName(type.genericArguments_[0], typeContext, string, package);
            return;
        case TypeType::Class:
        case TypeType::Protocol:
        case TypeType::Enum:
        case TypeType::ValueType:
            string.append(utf8(type.typeDefinition()->name()));
            break;
        case TypeType::MultiProtocol:
            string.append("🍱");
            for (auto &protocol : type.protocols()) {
                typeName(protocol, typeContext, string, package);
            }
            string.append("🍱");
            return;
        case TypeType::NoReturn:
            string.append("(no return)");
            return;
        case TypeType::Something:
            string.append("⚪️");
            return;
        case TypeType::Someobject:
            string.append("🔵");
            return;
        case TypeType::Callable:
            string.append("🍇");

            for (size_t i = 1; i < type.genericArguments_.size(); i++) {
                typeName(type.genericArguments_[i], typeContext, string, package);
            }

            if (type.genericArguments().front().type() != TypeType::NoReturn) {
                string.append("➡️");
                typeName(type.genericArguments().front(), typeContext, string, package);
            }
            string.append("🍉");
            return;
        case TypeType::Error:
            string.append("🚨");
            typeName(type.genericArguments_[0], typeContext, string, package);
            typeName(type.genericArguments_[1], typeContext, string, package);
            return;
        case TypeType::GenericVariable:
            if (typeContext.calleeType().canHaveGenericArguments()) {
                auto str = typeContext.calleeType().typeDefinition()->findGenericName(type.genericVariableIndex());
                string.append(utf8(str));
            }
            else {
                string.append("T" + std::to_string(type.genericVariableIndex()) + "?");
            }
            return;
        case TypeType::LocalGenericVariable:
            if (typeContext.function() != nullptr) {
                string.append(utf8(typeContext.function()->findGenericName(type.genericVariableIndex())));
            }
            else {
                string.append("L" + std::to_string(type.genericVariableIndex()) + "?");
            }
            return;
        case TypeType::StorageExpectation:
        case TypeType::Extension:
            return;
    }

    if (type.canHaveGenericArguments()) {
        for (size_t i = type.typeDefinition()->superGenericArguments().size(); i < type.genericArguments().size(); i++) {
            string.append("🐚");
            typeName(type.genericArguments()[i], typeContext, string, package);
        }
    }
}

std::string Type::toString(const TypeContext &typeContext, bool package) const {
    std::string string;
    typeName(*this, typeContext, string, package);
    return string;
}

}  // namespace EmojicodeCompiler
