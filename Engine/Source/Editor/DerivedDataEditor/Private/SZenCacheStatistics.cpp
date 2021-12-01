// Copyright Epic Games, Inc. All Rights Reserved.

#include "SZenCacheStatistics.h"
#include "ZenServerInterface.h"
#include "Math/UnitConversion.h"
#include "Math/BasicMathExpressionEvaluator.h"
#include "Misc/ExpressionParser.h"
#include "Styling/StyleColors.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Internationalization/FastDecimalFormat.h"

#define LOCTEXT_NAMESPACE "ZenEditor"

extern FString SingleDecimalFormat(double Value);
using namespace UE::Zen;

void SZenCacheStatisticsDialog::Construct(const FArguments& InArgs)
{
	const float RowMargin = 0.0f;
	const float TitleMargin = 10.0f;
	const float ColumnMargin = 10.0f;
	const FSlateColor TitleColour = FStyleColors::AccentWhite;
	const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 10);

	this->ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(0, 20, 0, 0)
		.AutoHeight()
		[
			SNew(SHorizontalBox)		
			+SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
				.ColorAndOpacity(TitleColour)
				.Font(TitleFont)
				.Justification(ETextJustify::Left)
				.Text(LOCTEXT("ZenStore", "ZenStore"))
			]		
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 5, 0, 0)
		.Expose(GridSlot)
		[
			GetGridPanel()
		]

	];

	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SZenCacheStatisticsDialog::UpdateGridPanels));
}

EActiveTimerReturnType SZenCacheStatisticsDialog::UpdateGridPanels(double InCurrentTime, float InDeltaTime)
{
	(*GridSlot)
	[
		GetGridPanel()
	];

	SlatePrepass(GetPrepassLayoutScaleMultiplier());

	return EActiveTimerReturnType::Continue;
}

TSharedRef<SWidget> SZenCacheStatisticsDialog::GetGridPanel()
{
	TSharedRef<SGridPanel> Panel = SNew(SGridPanel);

#if UE_WITH_ZEN

	FZenStats ZenStats;

	UE::Zen::GetDefaultServiceInstance().GetStats(ZenStats);

	double SumTotalGetMB = 0.0;
	double SumTotalPutMB = 0.0;
	double TotalUpstreamHitRatio = 0.0;

	for (const FZenEndPointStats& EndpointStats : ZenStats.UpstreamStats.EndPointStats)
	{
		SumTotalGetMB += EndpointStats.DownloadedMB;
		SumTotalPutMB += EndpointStats.UploadedMB;
		TotalUpstreamHitRatio += EndpointStats.HitRatio;
	}

	int32 Row = 0;

	const float RowMargin = 0.0f;
	const float TitleMargin = 10.0f;
	const float ColumnMargin = 10.0f;
	const FSlateColor TitleColor = FStyleColors::AccentWhite;
	const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 10);

	Panel->AddSlot(0, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Text(LOCTEXT("Cache", "Cache"))
		.Text(LOCTEXT("CacheType", "Cache Type"))
	];

	Panel->AddSlot(1, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Justification(ETextJustify::Left)
		.Text(LOCTEXT("Location", "Location"))
	];

	Panel->AddSlot(2, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Justification(ETextJustify::Left)
		.Text(LOCTEXT("HitPercentage", "Hit%"))
	];

	Panel->AddSlot(3, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Justification(ETextJustify::Left)
		.Text(LOCTEXT("Read", "Read"))
	];

	Panel->AddSlot(4, Row)
		[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Justification(ETextJustify::Left)
		.Text(LOCTEXT("Write", "Write"))
		];

	Panel->AddSlot(5, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin, 0.0f, TitleMargin))
			.ColorAndOpacity(TitleColor)
			.Font(TitleFont)
			.Justification(ETextJustify::Left)
			.Text(LOCTEXT("Details", "Details"))
		];
	Row++;

	Panel->AddSlot(0, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.Text(LOCTEXT("ZenServer", "Zen"))
	];

	Panel->AddSlot(1, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.Text(LOCTEXT("LocalServer", "Local"))
	];

	Panel->AddSlot(2, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.Text_Lambda([ZenStats, TotalUpstreamHitRatio] { return FText::FromString(SingleDecimalFormat( ( ZenStats.CacheStats.HitRatio- TotalUpstreamHitRatio ) * 100.0) + TEXT(" %")); })
	];

	Panel->AddSlot(5, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("%s:%d"), UE::Zen::GetDefaultServiceInstance().GetHostName(), UE::Zen::GetDefaultServiceInstance().GetPort() )); })
	];

	Row++;
	
	int32 EndpointIndex = 1;

	for (const FZenEndPointStats& EndpointStats : ZenStats.UpstreamStats.EndPointStats)
	{
		Panel->AddSlot(0, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin))
			.Text( EndpointStats.Name.Contains("Jupiter")? FText::FromString(TEXT("Horde")) : FText::FromString(TEXT("Zen")) )
		];

		Panel->AddSlot(1, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin))
			.Text(LOCTEXT("RemoteServer", "Remote"))
		];

		Panel->AddSlot(2, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin))
			.Text_Lambda([EndpointStats] { return FText::FromString(SingleDecimalFormat(EndpointStats.HitRatio * 100.0) + TEXT(" %")); })
		];

		Panel->AddSlot(3, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin))
			.Text_Lambda([EndpointStats] { return FText::FromString(SingleDecimalFormat(EndpointStats.DownloadedMB) + TEXT(" MB")); })
		];

		Panel->AddSlot(4, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin))
			.Text_Lambda([EndpointStats] { return FText::FromString(SingleDecimalFormat(EndpointStats.UploadedMB) + TEXT(" MB")); })
		];

		Panel->AddSlot(5, Row)
		[
			SNew(STextBlock)
			.Margin(FMargin(ColumnMargin, RowMargin))
			.Text_Lambda([EndpointStats] { return FText::FromString(EndpointStats.Name); })
		];

		Row++;
	}

	Panel->AddSlot(0, Row)
	[
		SNew(STextBlock)
		.Text(FText::FromString(TEXT("Total")))
		.Margin(FMargin(ColumnMargin, RowMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Justification(ETextJustify::Left)	
	];

	Panel->AddSlot(2, Row)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Text_Lambda([ZenStats] { return FText::FromString(SingleDecimalFormat( ZenStats.CacheStats.HitRatio * 100.0) + TEXT(" %")); })
	];

	Panel->AddSlot(3, Row)
	.HAlign(HAlign_Right)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.Justification(ETextJustify::Left)
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Text(FText::FromString(SingleDecimalFormat(SumTotalGetMB) + TEXT(" MB")))
		];

	Panel->AddSlot(4, Row)
	.HAlign(HAlign_Right)
	[
		SNew(STextBlock)
		.Margin(FMargin(ColumnMargin, RowMargin))
		.Justification(ETextJustify::Left)
		.ColorAndOpacity(TitleColor)
		.Font(TitleFont)
		.Text(FText::FromString(SingleDecimalFormat(SumTotalPutMB) + TEXT(" MB")))
	];
#endif

	return Panel;
}

#undef LOCTEXT_NAMESPACE
