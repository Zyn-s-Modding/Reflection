/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

/* How the blacklist's entries are drawn on the settings page.
 *
 * A type is a searchable list of every kind of asset there is, and a folder is a path box with the
 * Content Browser's folder picker beside it. Both still take whatever is typed into them, since the
 * game has types and folders this project has never heard of. */
struct REFLECTION_API FRBlacklistCustomization {
	static void Register();
	static void Unregister();
};

/* FRBlacklistType: one row, a dropdown of every asset type */
class FRBlacklistTypeCustomization : public IPropertyTypeCustomization {
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override {}
};

/* FRBlacklistFolder: one row, a path box and a folder picker */
class FRBlacklistFolderCustomization : public IPropertyTypeCustomization {
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override {}
};
