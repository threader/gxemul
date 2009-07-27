/*
 *  Copyright (C) 2008-2009  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright  
 *     notice, this list of conditions and the following disclaimer in the 
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE   
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 */

#include <assert.h>
#include <string.h>

#include "ComponentFactory.h"


struct ComponentListEntry {
	const char* componentName;
	refcount_ptr<Component> (*Create)();
	string (*GetAttribute)(const string& attributeName);
};

// Static list of components:
// (Note: components*.h is autogenerated by the configure script.)
#include "../../components_h.h"
static struct ComponentListEntry componentList[] = {
#include "../../components.h"
	{ NULL }
};

// List of components that are added dynamically at runtime:
static vector<ComponentListEntry> componentListRunTime;


bool ComponentFactory::RegisterComponentClass(const string& name,
	refcount_ptr<Component> (*createFunc)(),
	string (*getAttributeFunc)(const string& attributeName))
{
	// Attempt to create a component using this name first.
	// Don't add the new component class if the name is already in use.
	refcount_ptr<Component> component = CreateComponent(name);
	if (!component.IsNULL()) {
		assert(false);
		return false;
	}

	ComponentListEntry cle;
	cle.componentName = strdup(name.c_str());
	cle.Create = createFunc;
	cle.GetAttribute = getAttributeFunc;

	componentListRunTime.push_back(cle);

	return true;
}


refcount_ptr<Component> ComponentFactory::CreateComponent(
	const string& componentName)
{
	// Find the className in the list of available components, and
	// call the corresponding create function, if found:
	size_t i = 0;
	while (componentList[i].componentName != NULL) {
		if (componentName == componentList[i].componentName
#ifndef UNSTABLE_DEVEL
		    && !componentList[i].GetAttribute("stable").empty()
#endif
		    )
			return componentList[i].Create();

		++ i;
	}

	for (i=0; i<componentListRunTime.size(); ++i) {
		if (componentName == componentListRunTime[i].componentName
#ifndef UNSTABLE_DEVEL
		    && !componentListRunTime[i].GetAttribute("stable").empty()
#endif
		    )
			return componentListRunTime[i].Create();
	}

	return NULL;
}


string ComponentFactory::GetAttribute(const string& name,
	const string& attributeName)
{
	size_t i = 0;
	while (componentList[i].componentName != NULL) {
		if (name == componentList[i].componentName)
			return componentList[i].GetAttribute(attributeName);

		++ i;
	}
	
	for (i=0; i<componentListRunTime.size(); ++i) {
		if (name == componentListRunTime[i].componentName)
			return componentListRunTime[i].GetAttribute(
			    attributeName);
	}
	
	return "";
}


bool ComponentFactory::HasAttribute(const string& name,
	const string& attributeName)
{
	return !GetAttribute(name, attributeName).empty();
}


vector<string> ComponentFactory::GetAllComponentNames(bool onlyTemplates)
{
	vector<string> result;

	size_t i = 0;
	while (componentList[i].componentName != NULL) {
		if ((!onlyTemplates ||
		    componentList[i].GetAttribute("template") == "yes")
#ifndef UNSTABLE_DEVEL
		    && !componentList[i].GetAttribute("stable").empty()
#endif
		    )
			result.push_back(componentList[i].componentName);
		++ i;
	}

	for (i=0; i<componentListRunTime.size(); ++i) {
		if ((!onlyTemplates ||
		    componentListRunTime[i].GetAttribute("template") == "yes")
#ifndef UNSTABLE_DEVEL
		    && !componentListRunTime[i].GetAttribute("stable").empty()
#endif
		    )
			result.push_back(componentListRunTime[i].componentName);
	}

	return result;
}


/*****************************************************************************/


#ifdef WITHUNITTESTS

static void Test_ComponentFactory_Nonexistant()
{
	refcount_ptr<Component> component =
	    ComponentFactory::CreateComponent("NoNeXisT");
	UnitTest::Assert("nonexistant component should not be created",
	    component.IsNULL() == true);
}

static void Test_ComponentFactory_SimpleDummy()
{
	refcount_ptr<Component> component =
	    ComponentFactory::CreateComponent("dummy");
	UnitTest::Assert("dummy component should be possible to create",
	    component.IsNULL() == false);

	UnitTest::Assert("the class name should be 'dummy'",
	    component->GetClassName(), "dummy");
	UnitTest::Assert("the dummy component should have children",
	    component->GetChildren().size(), 0);
}

static void Test_ComponentFactory_FromTemplate()
{
	refcount_ptr<Component> component =
	    ComponentFactory::CreateComponent("mvme187");
	UnitTest::Assert("component should be possible to create from template",
	    component.IsNULL() == false);

	UnitTest::Assert("the class name of a mvme187 should be 'machine'",
	    component->GetClassName(), "machine");
	UnitTest::Assert("the mvme187 component should have children",
	    component->GetChildren().size() > 0);

	refcount_ptr<Component> clone = component->Clone();
	UnitTest::Assert("cloning should have been possible",
	    clone.IsNULL() == false);

	UnitTest::Assert("clone: the class name should still be 'machine'",
	    clone->GetClassName(), "machine");
	UnitTest::Assert("clone: the clone should also have children",
	    clone->GetChildren().size() > 0);
}

static void Test_ComponentFactory_HasAttribute()
{
	UnitTest::Assert("nonexistantattr should not exist",
	    !ComponentFactory::HasAttribute("testm88k", "nonexistantattr"));

	UnitTest::Assert("testm88k is a machine",
	    ComponentFactory::HasAttribute("testm88k", "machine"));

	UnitTest::Assert("testm88k is stable",
	    ComponentFactory::HasAttribute("testm88k", "stable"));

	UnitTest::Assert("testm88k has a description",
	    ComponentFactory::HasAttribute("testm88k", "description"));
}

UNITTESTS(ComponentFactory)
{
	UNITTEST(Test_ComponentFactory_Nonexistant);
	UNITTEST(Test_ComponentFactory_SimpleDummy);
	UNITTEST(Test_ComponentFactory_FromTemplate);
	UNITTEST(Test_ComponentFactory_HasAttribute);
}

#endif
