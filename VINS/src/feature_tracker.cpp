#include "feature_tracker.h"

//https://blog.csdn.net/qq_41839222/article/details/85797156
int FeatureTracker::n_id = 0;

bool inBorder(const cv::Point2f &pt) {
    const int BORDER_SIZE = 1;
    int img_x = cvRound(pt.x);
    int img_y = cvRound(pt.y);
    return BORDER_SIZE <= img_x && img_x < COL - BORDER_SIZE && BORDER_SIZE <= img_y && img_y < ROW - BORDER_SIZE;
}

void reduceVector(vector <cv::Point2f> &v, vector <uchar> status) {
    int j = 0;
    for (int i = 0; i < int(v.size()); i++)
        if (status[i])
            v[j++] = v[i];
    v.resize(j);//扩充v的大小 这里的意思是去掉部分不要的点之后，修改v的大小（v变小了）---保留前j个
}

void reduceVector(vector<int> &v, vector <uchar> status) {
    int j = 0;
    for (int i = 0; i < int(v.size()); i++)
        if (status[i])
            v[j++] = v[i];
    v.resize(j);
}


FeatureTracker::FeatureTracker() {
}

/**
 * @brief 通过设置一个mask，使跟踪的特征点在整幅图像中能够均匀分布，防止特征点扎堆。
 * 具体操作为：对光流跟踪到的特征点forw_pts，按照被跟踪到的次数降序排列，然后按照降序遍历这些特征点。
 * 每选中一个特征点，在mask中将该点周围半径为MIN_DIST的区域设置为0，后面不再选取该区域内的特征点。
 * 这样会删去一些特征点，使得特征点分布得更加均匀，同时尽可能地保留被跟踪次数更多的特征点。
 */
void FeatureTracker::setMask() {
    if (FISHEYE)
        mask = fisheye_mask.clone();
    else
        mask = cv::Mat(ROW, COL, CV_8UC1, cv::Scalar(255));


    // prefer to keep features that are tracked for long time
    vector < pair < int, pair < cv::Point2f, int>>> cnt_pts_id;

    for (unsigned int i = 0; i < forw_pts.size(); i++)
        cnt_pts_id.push_back(make_pair(track_cnt[i], make_pair(forw_pts[i], ids[i])));

    sort(cnt_pts_id.begin(), cnt_pts_id.end(),
         [](const pair<int, pair<cv::Point2f, int>> &a, const pair<int, pair<cv::Point2f, int>> &b) {
             return a.first > b.first;
         });

    forw_pts.clear();
    ids.clear();
    track_cnt.clear();

    for (auto &it : cnt_pts_id) {
        if (mask.at<uchar>(it.second.first) == 255) {
            forw_pts.push_back(it.second.first);
            ids.push_back(it.second.second);
            track_cnt.push_back(it.first);
            cv::circle(mask, it.second.first, MIN_DIST, 0, -1);
        }
    }
}

void FeatureTracker::addPoints() {
    for (auto &p : n_pts) {
        forw_pts.push_back(p);
        ids.push_back(-1);
        track_cnt.push_back(1);
    }
}

void FeatureTracker::readImage(const cv::Mat &_img, double _cur_time) {
    cv::Mat img;
    TicToc t_r;
    cur_time = _cur_time;
    //if image is too dark or light, trun on equalize to find enough features
    if (EQUALIZE) {
        cv::Ptr <cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(8, 8));
        TicToc t_c;
        clahe->apply(_img, img);//自适应直方图均衡
        //ROS_DEBUG("CLAHE costs: %fms", t_c.toc());
    } else
        img = _img;

    if (forw_img.empty()) {
        prev_img = cur_img = forw_img = img;
    } else {
        forw_img = img;
    }
    //vector<cv::Point2f>
    forw_pts.clear();

    if (cur_pts.size() > 0) {
        TicToc t_o;
        vector <uchar> status;
        vector<float> err;
        //光流跟踪
        //status为1或0,表示特征点是否被跟踪到
        cv::calcOpticalFlowPyrLK(cur_img, forw_img, cur_pts, forw_pts, status, err, cv::Size(21, 21), 3);

        for (int i = 0; i < int(forw_pts.size()); i++)
            if (status[i] && !inBorder(forw_pts[i])) //判断跟踪的特征点是否在图像边界内
                status[i] = 0;
        //去除status为0的数据
        reduceVector(prev_pts, status);
        reduceVector(cur_pts, status);
        reduceVector(forw_pts, status);
        reduceVector(ids, status);//直接根据status的值，按照顺序进行赋值
        reduceVector(cur_un_pts, status);
        reduceVector(track_cnt, status);
        //ROS_DEBUG("temporal optical flow costs: %fms", t_o.toc());
    }

    for (auto &n : track_cnt)
        n++;

    if (PUB_THIS_FRAME) {
        //通过F矩阵去除outliers(包括从uv恢复到归一化坐标)
        rejectWithF();
        //ROS_DEBUG("set mask begins");
        TicToc t_m;
        //对跟踪点进行排序并去除密集点
        setMask();
        //ROS_DEBUG("set mask costs %fms", t_m.toc());

        //ROS_DEBUG("detect feature begins");
        TicToc t_t;
        //MAX_CNT=max_cnt: 150 一帧图像最多跟踪150个特征点
        int n_max_cnt = MAX_CNT - static_cast<int>(forw_pts.size());
        if (n_max_cnt > 0) {
            if (mask.empty())
                cout << "mask is empty " << endl;
            if (mask.type() != CV_8UC1)
                cout << "mask type wrong " << endl;
            if (mask.size() != forw_img.size())
                cout << "wrong size " << endl;
            cv::goodFeaturesToTrack(forw_img, n_pts, MAX_CNT - forw_pts.size(), 0.01, MIN_DIST, mask);
        } else
            n_pts.clear();
        //ROS_DEBUG("detect feature costs: %fms", t_t.toc());

        //ROS_DEBUG("add feature begins");
        TicToc t_a;
        addPoints();//添将新检测到的特征点n_pts，ID初始化-1，跟踪次数1
        //ROS_DEBUG("selectFeature costs: %fms", t_a.toc());
    }
    prev_img = cur_img;
    prev_pts = cur_pts;
    prev_un_pts = cur_un_pts;
    cur_img = forw_img;
    cur_pts = forw_pts;
    //对特征点的图像坐标根据不同的相机模型进行去畸变矫正和深度归一化，计算每个角点的速度
    undistortedPoints();
    prev_time = cur_time;
}

/**
 * @brief 剔除outliers。具体方法为：调用cv::findFundamentalMat()对prev_pts和forw_pts计算F矩阵，通过F矩阵去除outliers。剩下的特征点track_cnt都加1。
 */
void FeatureTracker::rejectWithF() {
    if (forw_pts.size() >= 8) {
        //ROS_DEBUG("FM ransac begins");
        TicToc t_f;
        vector <cv::Point2f> un_cur_pts(cur_pts.size()), un_forw_pts(forw_pts.size());
        for (unsigned int i = 0; i < cur_pts.size(); i++) {
            Eigen::Vector3d tmp_p;
            //从uv坐标系恢复到归一化坐标系
            //根据不同的相机模型将二维坐标转换到三维坐标
            //对于PINHOLE（针孔相机）可将像素坐标转换到归一化平面并去畸变
            //对于CATA（卡特鱼眼相机）将像素坐标投影到单位圆内
            //恢复出来之后我管你准不准，归一化之后都是一样的
            m_camera->liftProjective(Eigen::Vector2d(cur_pts[i].x, cur_pts[i].y), tmp_p);
            //深度归一化
            tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
            tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
            un_cur_pts[i] = cv::Point2f(tmp_p.x(), tmp_p.y());

            m_camera->liftProjective(Eigen::Vector2d(forw_pts[i].x, forw_pts[i].y), tmp_p);
            tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
            tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
            un_forw_pts[i] = cv::Point2f(tmp_p.x(), tmp_p.y());
        }

        vector <uchar> status;
        cv::findFundamentalMat(un_cur_pts, un_forw_pts, cv::FM_RANSAC, F_THRESHOLD, 0.99, status);
        int size_a = cur_pts.size();
        reduceVector(prev_pts, status);
        reduceVector(cur_pts, status);
        reduceVector(forw_pts, status);
        reduceVector(cur_un_pts, status);
        reduceVector(ids, status);
        reduceVector(track_cnt, status);
        //ROS_DEBUG("FM ransac: %d -> %lu: %f", size_a, forw_pts.size(), 1.0 * forw_pts.size() / size_a);
        //ROS_DEBUG("FM ransac costs: %fms", t_f.toc());
    }
}

bool FeatureTracker::updateID(unsigned int i) {
    if (i < ids.size()) {
        if (ids[i] == -1)
            ids[i] = n_id++;
        return true;
    } else
        return false;
}

void FeatureTracker::readIntrinsicParameter(const string &calib_file) {
    cout << "reading paramerter of camera " << calib_file << endl;
    m_camera = CameraFactory::instance()->generateCameraFromYamlFile(calib_file);
}

void FeatureTracker::showUndistortion(const string &name) {
    cv::Mat undistortedImg(ROW + 600, COL + 600, CV_8UC1, cv::Scalar(0));
    vector <Eigen::Vector2d> distortedp, undistortedp;
    for (int i = 0; i < COL; i++)
        for (int j = 0; j < ROW; j++) {
            Eigen::Vector2d a(i, j);
            Eigen::Vector3d b;
            m_camera->liftProjective(a, b);
            distortedp.push_back(a);
            undistortedp.push_back(Eigen::Vector2d(b.x() / b.z(), b.y() / b.z()));
            //printf("%f,%f->%f,%f,%f\n)\n", a.x(), a.y(), b.x(), b.y(), b.z());
        }
    for (int i = 0; i < int(undistortedp.size()); i++) {
        cv::Mat pp(3, 1, CV_32FC1);
        pp.at<float>(0, 0) = undistortedp[i].x() * FOCAL_LENGTH + COL / 2;
        pp.at<float>(1, 0) = undistortedp[i].y() * FOCAL_LENGTH + ROW / 2;
        pp.at<float>(2, 0) = 1.0;
        //cout << trackerData[0].K << endl;
        //printf("%lf %lf\n", p.at<float>(1, 0), p.at<float>(0, 0));
        //printf("%lf %lf\n", pp.at<float>(1, 0), pp.at<float>(0, 0));
        if (pp.at<float>(1, 0) + 300 >= 0 && pp.at<float>(1, 0) + 300 < ROW + 600 && pp.at<float>(0, 0) + 300 >= 0 &&
            pp.at<float>(0, 0) + 300 < COL + 600) {
            undistortedImg.at<uchar>(pp.at<float>(1, 0) + 300, pp.at<float>(0, 0) + 300) = cur_img.at<uchar>(
                    distortedp[i].y(), distortedp[i].x());
        } else {
            //ROS_ERROR("(%f %f) -> (%f %f)", distortedp[i].y, distortedp[i].x, pp.at<float>(1, 0), pp.at<float>(0, 0));
        }
    }
//    cv::imshow(name, undistortedImg);
//    cv::waitKey(0);
}

void FeatureTracker::undistortedPoints() {
    cur_un_pts.clear();
    cur_un_pts_map.clear();
    //cv::undistortPoints(cur_pts, un_pts, K, cv::Mat());
    for (unsigned int i = 0; i < cur_pts.size(); i++) {
        Eigen::Vector2d a(cur_pts[i].x, cur_pts[i].y);
        Eigen::Vector3d b;
        m_camera->liftProjective(a, b);
        cur_un_pts.push_back(cv::Point2f(b.x() / b.z(), b.y() / b.z()));
        cur_un_pts_map.insert(make_pair(ids[i], cv::Point2f(b.x() / b.z(), b.y() / b.z())));
        //printf("cur pts id %d %f %f", ids[i], cur_un_pts[i].x, cur_un_pts[i].y);
    }
    // caculate points velocity
    if (!prev_un_pts_map.empty()) {
        double dt = cur_time - prev_time;
        pts_velocity.clear();
        for (unsigned int i = 0; i < cur_un_pts.size(); i++) {
            if (ids[i] != -1) {
                std::map<int, cv::Point2f>::iterator it;
                it = prev_un_pts_map.find(ids[i]);
                if (it != prev_un_pts_map.end()) {
                    double v_x = (cur_un_pts[i].x - it->second.x) / dt;
                    double v_y = (cur_un_pts[i].y - it->second.y) / dt;
                    pts_velocity.push_back(cv::Point2f(v_x, v_y));
                } else
                    pts_velocity.push_back(cv::Point2f(0, 0));
            } else {
                pts_velocity.push_back(cv::Point2f(0, 0));
            }
        }
    } else {
        for (unsigned int i = 0; i < cur_pts.size(); i++) {
            pts_velocity.push_back(cv::Point2f(0, 0));
        }
    }
    prev_un_pts_map = cur_un_pts_map;
}
