// Copyright Epic Games, Inc. All Rights Reserved.
#include "UI/BridgeUIManager.h"
#include "UI/FBridgeMessageHandler.h"
#include "UI/BridgeStyle.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "NodePort.h"
#include "NodeProcess.h"

// WebBrowser
#include "SWebBrowser.h"
#include "WebBrowserModule.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserCookieManager.h"
// Widgets
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SScrollBox.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "Misc/MessageDialog.h"
#include "ContentBrowserDataMenuContexts.h"

#define LOCTEXT_NAMESPACE "Bridge"
#define LEVELEDITOR_MODULE_NAME TEXT("LevelEditor")
#define CONTENTBROWSER_MODULE_NAME TEXT("ContentBrowser")

TSharedPtr<FBridgeUIManagerImpl> FBridgeUIManager::Instance;
UBrowserBinding* FBridgeUIManager::BrowserBinding;

const FName BridgeTabName = "BridgeTab";

void FBridgeUIManager::Initialize()
{
	if (!Instance.IsValid())
	{
		//Instance = MakeUnique<FBridgeUIManagerImpl>();
		Instance = MakeShareable(new FBridgeUIManagerImpl);
		Instance->Initialize();
	}
}

void FBridgeUIManagerImpl::Initialize()
{
	FBridgeStyle::Initialize();
	SetupMenuItem();
}

void FBridgeUIManagerImpl::SetupMenuItem()
{
	FBridgeStyle::SetIcon("Logo", "Logo80x80");
	FBridgeStyle::SetIcon("ContextLogo", "Logo32x32");
	FBridgeStyle::SetSVGIcon("MenuLogo", "QuixelBridgeB");
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(LEVELEDITOR_MODULE_NAME);
	TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
	ToolbarExtender->AddToolBarExtension("Settings", EExtensionHook::After, nullptr, FToolBarExtensionDelegate::CreateRaw(this, &FBridgeUIManagerImpl::FillToolbar));
	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);

	// For Deleting Cookies
	// IWebBrowserSingleton* WebBrowserSingleton = IWebBrowserModule::Get().GetSingleton();
	// if (WebBrowserSingleton)
	// {
	// 	TSharedPtr<IWebBrowserCookieManager> CookieManager = WebBrowserSingleton->GetCookieManager();
	// 	if (CookieManager.IsValid())
	// 	{
	// 		CookieManager->DeleteCookies();
	// 	}
	// }

	// Adding Bridge entry to Quick Content menu.
	UToolMenu* AddMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.AddQuickMenu");
	FToolMenuSection& Section = AddMenu->FindOrAddSection("Content");
	Section.AddMenuEntry("OpenBridgeTab",
		LOCTEXT("OpenBridgeTab_Label", "Quixel Bridge"),
		LOCTEXT("OpenBridgeTab_Desc", "Opens the Quixel Bridge."),
		FSlateIcon(FBridgeStyle::GetStyleSetName(), "Bridge.MenuLogo"),
		FUIAction(FExecuteAction::CreateRaw(this, &FBridgeUIManagerImpl::CreateWindow), FCanExecuteAction())
	);

	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("MainFrame.MainMenu.Window");
	FToolMenuSection* ContentSectionPtr = WindowMenu->FindSection("GetContent");
	if (!ContentSectionPtr)
	{
		ContentSectionPtr = &WindowMenu->AddSection("GetContent", NSLOCTEXT("MainAppMenu", "GetContentHeader", "Get Content"));
	}
	ContentSectionPtr->AddMenuEntry("OpenBridgeTab",
		LOCTEXT("OpenBridgeTab_Label", "Quixel Bridge"),
		LOCTEXT("OpenBridgeTab_Desc", "Opens the Quixel Bridge."),
		FSlateIcon(FBridgeStyle::GetStyleSetName(), "Bridge.MenuLogo"),
		FUIAction(FExecuteAction::CreateRaw(this, &FBridgeUIManagerImpl::CreateWindow), FCanExecuteAction())
	);
	//Section.AddSeparator(NAME_None);

	//Adding Bridge entry to Content Browser context and New menu.
	UToolMenu* ContextMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AddNewContextMenu");
	//FToolMenuSection& ContextMenuSection = ContextMenu->AddSection("ContentBrowserMegascans", LOCTEXT("GetContentMenuHeading", "Quixel Content"));
	FToolMenuSection& ContextMenuSection = ContextMenu->FindOrAddSection("ContentBrowserGetContent");
	
	TWeakPtr<FBridgeUIManagerImpl> WeakPtr = AsShared();
	ContextMenuSection.AddDynamicEntry("GetMegascans", FNewToolMenuSectionDelegate::CreateLambda([WeakPtr](FToolMenuSection& InSection)
	{
		UContentBrowserDataMenuContext_AddNewMenu* AddNewMenuContext = InSection.FindContext<UContentBrowserDataMenuContext_AddNewMenu>();
		if (AddNewMenuContext && AddNewMenuContext->bCanBeModified && AddNewMenuContext->bContainsValidPackagePath && WeakPtr.IsValid())
		{
			InSection.AddMenuEntry(
				"GetMegascans",
				LOCTEXT("OpenBridgeTabText", "Add Quixel Content"),
				LOCTEXT("GetBridgeTooltip", "Add Megascans and DHI assets to project."),
				FSlateIcon(FBridgeStyle::GetStyleSetName(), "Bridge.MenuLogo"),
				FUIAction(FExecuteAction::CreateSP(WeakPtr.Pin().ToSharedRef(), &FBridgeUIManagerImpl::CreateWindow), FCanExecuteAction())
			);
		}
	}));

	/*TSharedPtr<FExtender> NewMenuExtender = MakeShareable(new FExtender);
	NewMenuExtender->AddMenuExtension("LevelEditor",
		EExtensionHook::After,
		NULL,
		FMenuExtensionDelegate::CreateRaw(this, &FBridgeUIManagerImpl::AddPluginMenu));
	LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(NewMenuExtender);*/
	
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(BridgeTabName,
	FOnSpawnTab::CreateRaw(this, &FBridgeUIManagerImpl::CreateBridgeTab))
		.SetDisplayName(BridgeTabDisplay)
		.SetAutoGenerateMenuEntry(false)
		.SetTooltipText(BridgeToolTip);
}

void FBridgeUIManagerImpl::AddPluginMenu(FMenuBuilder& MenuBuilder)
{
	MenuBuilder.BeginSection("CustomMenu", TAttribute<FText>(FText::FromString("Quixel")));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("OpenWindow", "Quixel Bridge"),
		LOCTEXT("ToolTip", "Open Quixel Bridge"),
		FSlateIcon(FBridgeStyle::GetStyleSetName(), "Bridge.Logo"),
		FUIAction(FExecuteAction::CreateRaw(this, &FBridgeUIManagerImpl::CreateWindow))
	);

	MenuBuilder.EndSection();
}

void FBridgeUIManagerImpl::FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.BeginSection(TEXT("QuixelBridge"));
	{
		ToolbarBuilder.AddToolBarButton(
			FUIAction(FExecuteAction::CreateRaw(this, &FBridgeUIManagerImpl::CreateWindow)),
			FName(TEXT("Quixel Bridge")),
			LOCTEXT("QMSLiveLink_label", "Bridge"),
			LOCTEXT("WorldProperties_ToolTipOverride", "Megascans Link with Bridge"),
			FSlateIcon(FBridgeStyle::GetStyleSetName(), "Bridge.Logo"),
			EUserInterfaceActionType::Button,
			FName(TEXT("QuixelBridge"))
		);
	}
	ToolbarBuilder.EndSection();
}

void FBridgeUIManagerImpl::CreateWindow()
{
	FGlobalTabmanager::Get()->TryInvokeTab(BridgeTabName);

	// Set desired window size (if the desired window size is less than main window size)
	// Rationale: the main window is mostly maximized - so the size is equal to screen size
	TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetTopLevelWindows();
	if (Windows.Num() > 0)
	{
		FVector2D MainWindowSize = Windows[0]->GetSizeInScreen();
		float DesiredWidth = 1650;
		float DesiredHeight = 900;

		if (DesiredWidth < MainWindowSize.X && DesiredHeight < MainWindowSize.Y)
		{
			LocalBrowserDock->GetParentWindow()->Resize(FVector2D(DesiredWidth, DesiredHeight));
			LocalBrowserDock->GetParentWindow()->MoveWindowTo(FVector2D((MainWindowSize.X - DesiredWidth) - 17, MainWindowSize.Y - DesiredHeight) / 2);
		}
	}
}

void FBridgeUIManager::Shutdown()
{
	if (FBridgeUIManager::Instance.IsValid())
	{
		if (FBridgeUIManager::Instance->Browser != NULL && FBridgeUIManager::Instance->Browser.IsValid())
		{
			FBridgeUIManager::Instance->Browser = NULL;
		}
		if (FBridgeUIManager::Instance->WebBrowserWidget != NULL && FBridgeUIManager::Instance->WebBrowserWidget.IsValid())
		{
			FBridgeUIManager::Instance->WebBrowserWidget = NULL;
		}
		if (FBridgeUIManager::Instance->LocalBrowserDock != NULL && FBridgeUIManager::Instance->LocalBrowserDock.IsValid())
		{
			FBridgeUIManager::Instance->LocalBrowserDock = NULL;
		}
	}

	FBridgeStyle::Shutdown();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(BridgeTabName);
}

TSharedRef<SDockTab> FBridgeUIManagerImpl::CreateBridgeTab(const FSpawnTabArgs& Args)
{
	// Start node process
	FNodeProcessManager::Get()->StartNodeProcess();

	// Delay launch on Mac & Linux
#if PLATFORM_MAC || PLATFORM_LINUX
	FGenericPlatformProcess::Sleep(2);
#endif

	FWebBrowserInitSettings browserInitSettings = FWebBrowserInitSettings();
	IWebBrowserModule::Get().CustomInitialize(browserInitSettings);

	FString PluginPath = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("Bridge"));
	FString IndexUrl = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginPath, TEXT("ThirdParty"), TEXT("megascans"), TEXT("index.html")));

	WindowSettings.InitialURL = FPaths::Combine(TEXT("file:///"), IndexUrl);
	WindowSettings.BrowserFrameRate = 60;

	IWebBrowserSingleton* WebBrowserSingleton = IWebBrowserModule::Get().GetSingleton();

	if (IWebBrowserModule::IsAvailable() && IWebBrowserModule::Get().IsWebModuleAvailable())
	{
		Browser = WebBrowserSingleton->CreateBrowserWindow(WindowSettings);
		SAssignNew(LocalBrowserDock, SDockTab)
			.OnTabClosed_Lambda([](TSharedRef<class SDockTab> InParentTab)
			{
				// Kill node process if bound
				FBridgeUIManager::BrowserBinding->OnExitDelegate.ExecuteIfBound("Plugin Window Closed");
				FBridgeUIManager::BrowserBinding = NULL;
				// Adding a delay for Mac, for node process to exit completely (before plugin)
			#if PLATFORM_MAC
				FGenericPlatformProcess::Sleep(0.3);
			#endif

				// Clean up browser
				FBridgeUIManager::Instance->LocalBrowserDock = NULL;
				if (FBridgeUIManager::Instance->WebBrowserWidget.IsValid())
				{
					FBridgeUIManager::Instance->WebBrowserWidget.Reset();
					FBridgeUIManager::Instance->Browser.Reset();
				}
			})
			.TabRole(ETabRole::NomadTab)
			[
				SAssignNew(WebBrowserWidget, SWebBrowser, Browser)
				.ShowAddressBar(false)
				.ShowControls(false)
			];

		LocalBrowserDock->SetOnTabDraggedOverDockArea(
			FSimpleDelegate::CreateLambda([IndexUrl]()
										  {
											FBridgeUIManager::Instance->WebBrowserWidget->Invalidate(EInvalidateWidgetReason::Layout);
										  })
		);
		LocalBrowserDock->SetOnTabRelocated(
			FSimpleDelegate::CreateLambda([IndexUrl]()
										  {
											FBridgeUIManager::Instance->WebBrowserWidget->Invalidate(EInvalidateWidgetReason::Layout);
										  })
		);
	}

	if (WebBrowserWidget.IsValid())
	{
		UNodePort* NodePortInfo = NewObject<UNodePort>();
		FBridgeUIManager::BrowserBinding = NewObject<UBrowserBinding>();
		FBridgeUIManager::Instance->WebBrowserWidget->BindUObject(TEXT("NodePortInfo"), NodePortInfo, true);
		FBridgeUIManager::Instance->WebBrowserWidget->BindUObject(TEXT("BrowserBinding"), FBridgeUIManager::BrowserBinding, true);
	}

	if (LocalBrowserDock.IsValid())
	{
		return LocalBrowserDock.ToSharedRef();
	}

	return SAssignNew(LocalBrowserDock, SDockTab);
}

#undef LOCTEXT_NAMESPACE
