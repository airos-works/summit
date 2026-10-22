// Native PDF printing regression: compare the loaded document with print output.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"

class PrintProbe final : public BApplication {
public:
    PrintProbe(char** argv) : BApplication("application/x-vnd.Summit-PDFPrintProbe"),
        url(argv[1]), output(argv[2]), bundle(argv[3]), gate(argv[4]), baseline(std::string(argv[5])=="baseline") { }
    int Result() const { return failed; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            Require(BWebKitInitialize()==B_OK,"initialize WebKit");
            context=std::make_shared<BWebKitContext>((output+"/profile").c_str());
            window=new BWindow(BRect(40,40,1000,800),"PDF print probe",B_TITLED_WINDOW,B_ASYNCHRONOUS_CONTROLS);
            view=new BWebKitView(window->Bounds(),"pdf",BMessenger(this),B_FOLLOW_ALL,context);
            window->AddChild(view); window->Show();
            Require(view->InitCheck()==B_OK,"native view initializes");
            worker=std::thread([this] {
                try { RunChecks(); }
                catch(const std::exception& e){std::fprintf(stderr,"PDF_PRINT_FAILURE %s\n",e.what());failed=true;}
                done=true;
            });
        } catch(const std::exception& e){std::fprintf(stderr,"PDF_PRINT_FAILURE %s\n",e.what());failed=done=true;}
    }
    void MessageReceived(BMessage* message) override
    {
        if(message->what==B_WEBKIT_INSPECTOR_MESSAGES)return;
        if(message->what==B_WEBKIT_STATE_CHANGED){std::lock_guard lock(mutex);state=*message;return;}
        if(message->what==B_WEBKIT_PRINT_PAGES){std::lock_guard lock(mutex);replies[message->GetUInt64("identifier",0)]=*message;return;}
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if(!done)return;
        if(!closing){closing=system_time();if(worker.joinable())worker.join();session.reset();if(window->Lock())window->Quit();context.reset();}
        if(!HasHelpers(bundle)&&!BWebKitHasPendingNativeUI()){
            std::printf("PDF_PRINT_RESULT %s checks=%u\n",failed?"FAIL":"PASS",checks);finished=true;PostMessage(B_QUIT_REQUESTED);
        }else if(system_time()-closing>30000000){std::fputs("PDF_PRINT_FAILURE teardown\n",stderr);_Exit(1);}
    }
    bool QuitRequested() override{return finished;}
private:
    void Wait(std::function<bool()> f,const char* label,bigtime_t timeout=30000000)
    {
        auto until=system_time()+timeout;
        while(system_time()<until){if(f()){Require(true,label);return;}snooze(50000);}Require(false,label);
    }
    BMessage Print(uint64 id,float raster,uint32 first,uint32 count)
    {
        view->DrawPagesForPrinting(BMessenger(this),id,595,842,BRect(36,36,36,36),1,first,count,raster,true);
        Wait([&]{std::lock_guard lock(mutex);return replies.contains(id);},"native print request returns",60000000);
        std::lock_guard lock(mutex);return replies.at(id);
    }
    void RunChecks()
    {
        view->LoadURL(url.c_str());
        Wait([&]{std::lock_guard lock(mutex);return state.GetString("url","")==url&&!state.GetBool("loading",true);},"PDF navigation completes");
        Require(window->Lock(),"lock for Inspector");session=BWebKitInspectorSession::Create(*view,BMessenger(this));window->Unlock();
        Require(bool(session),"Inspector attaches");Inspector inspector(session);unsigned viewer=0;
        Wait([&]{viewer=inspector.Viewer();return viewer&&inspector.Evaluate("PDFViewerApplication.pdfDocument?.numPages",viewer)==3;},"source PDF has three pages");
        Wait([&]{return inspector.Evaluate("PDFViewerApplication.pdfViewer.getPageView(0)?.renderingState===3",viewer)==true;},"first PDF page rendered");
        JSON result;
        result["viewerPages"]=inspector.Evaluate("PDFViewerApplication.pdfDocument.numPages",viewer);
        result["toolbarPrintDisplay"]=inspector.Evaluate("getComputedStyle(document.getElementById('printButton')).display",viewer);
        result["supportsPrinting"]=inspector.Evaluate("PDFViewerApplication.supportsPrinting",viewer);
        std::puts("PDF_PRINT_READY");std::fflush(stdout);
        Wait([&]{return std::filesystem::exists(gate);},"runtime inventory gate",180000000);
        auto pdf=Print(1,0,0,1000);
        result["nativePDFPageCount"]=pdf.GetInt32("pageCount",-1);result["nativePDFError"]=pdf.GetString("error","");
        const void* data=nullptr;ssize_t size=0;
        Require(pdf.FindData("pdf",B_RAW_TYPE,&data,&size)==B_OK&&size>100,"native print produces a PDF");
        std::ofstream file(output+"/printed.pdf",std::ios::binary);file.write(static_cast<const char*>(data),size);file.close();
        result["nativePDFBytes"]=size;
        auto raster=Print(2,1,0,3);
        result["nativeRasterPageCount"]=raster.GetInt32("pageCount",-1);result["nativeRasterError"]=raster.GetString("error","");
        int width=raster.GetInt32("width",0),height=raster.GetInt32("height",0);
        result["rasterSize"]={width,height};
        int32 rasterPages=0;
        for(int32 index=0;raster.FindData("page",B_RAW_TYPE,index,&data,&size)==B_OK;++index){
            ++rasterPages;
            Require(size==ssize_t(width)*height*4,"raster page byte length matches dimensions");
            std::ofstream ppm(output+"/printed-"+std::to_string(index)+".ppm",std::ios::binary);
            ppm<<"P6\n"<<width<<' '<<height<<"\n255\n";
            auto* p=static_cast<const unsigned char*>(data);
            for(ssize_t i=0;i<size;i+=4){char rgb[]={char(p[i+2]),char(p[i+1]),char(p[i])};ppm.write(rgb,3);}
        }
        result["rasterPages"]=rasterPages;
        auto beyond=Print(3,1,1,1);
        result["secondPageCount"]=beyond.GetInt32("pageCount",-1);result["secondPageHasData"]=beyond.HasData("page",B_RAW_TYPE);
        if(baseline) {
            Require(result["toolbarPrintDisplay"]=="none"&&!result["supportsPrinting"].get<bool>(),"baseline hides and disables PDF printing");
            Require(result["nativePDFPageCount"]==1&&result["nativeRasterPageCount"]==1&&rasterPages==1&&!result["secondPageHasData"].get<bool>(),"baseline prints one viewer sheet instead of three PDF pages");
        } else {
            Require(result["toolbarPrintDisplay"]!="none"&&result["supportsPrinting"].get<bool>(),"PDF viewer offers printing");
            Require(result["nativePDFPageCount"]==3&&result["nativeRasterPageCount"]==3&&rasterPages==3&&result["secondPageCount"]==3&&result["secondPageHasData"].get<bool>(),"native printing includes all three document pages");
        }
        std::ofstream(output+"/observed.json")<<result.dump(2)<<'\n';
        std::printf("PDF_PRINT_OBSERVED %s\n",result.dump().c_str());
    }
    std::string url,output,bundle,gate;bool baseline;
    BWindow* window=nullptr;BWebKitView* view=nullptr;std::shared_ptr<BWebKitContext> context;
    std::shared_ptr<BWebKitInspectorSession> session;std::thread worker;std::mutex mutex;BMessage state;std::map<uint64,BMessage> replies;
    std::atomic<bool> failed=false,done=false;bool finished=false;bigtime_t closing=0;
};
int main(int argc,char**argv)
{
    if(argc!=6){std::fputs("Usage: ModernPDFPrintTests URL OUTPUT BUNDLE GATE baseline|candidate\n",stderr);return 2;}
    PrintProbe app(argv);app.Run();return app.Result();
}
