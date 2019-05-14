#include "ldes_tracker.h"

LDESTracker::LDESTracker()
{
	lambda = 0.0001;
	padding = 2.5;
	scale_padding = 2.5;
	output_sigma_factor = 0.1;

	_hogfeatures = true;
	_rotation = true;
	_scale_hog = false;

	interp_factor = 0.012;
	interp_n = 0.85;
	sigma = 0.6;

	cell_size = 4;
	inter_patch_rate = 0.3;
	color_update_rate = 0.01;
	color_bins = 10;
	merge_factor = 0.4;
	_rotation = false;
}


LDESTracker::~LDESTracker()
{
}

void LDESTracker::init(const cv::Rect &roi, cv::Mat image) {
	cell_size = 4;

	_roi = roi;
	target_sz = roi.size();
	cur_position = _roi;
	cur_rot_degree = 0.;
	cur_scale;

	assert(roi.width >= 0 && roi.height >= 0);

	cur_pos.x = roi.x + roi.width*0.5;
	cur_pos.y = roi.y + roi.height*0.5;
	cur_roi = roi;

	//for cropping, then resize to window_sz0
	window_sz = static_cast<int>(sqrt(target_sz.area())*padding);	
	cur_scale = 1.0;

	float tmp;
	float search_area = 1.0*window_sz*window_sz;
	if (search_area > max_area)
		tmp = sqrt(search_area / max_area);
	else if (search_area < min_area)
		tmp = sqrt(search_area / min_area);
	else
		tmp = 1.0;

	//within range [100,350], for template
	window_sz0 = static_cast<int>(1.0*window_sz / tmp);
	//crop area feature size
	feature_sz = static_cast<int>(1.0*window_sz / cell_size);
	//quantified template size
	window_sz0 = feature_sz * cell_size;
	//adjust scale
	//cur_scale = 1.0*window_sz / window_sz0;
	//scaled target sz
	target_sz0 = target_sz;
	//target_sz0.width = target_sz.width / cur_scale;
	//target_sz0.height = target_sz.height / cur_scale;

	int avg_dim = window_sz / 4;
	window_sz_search = window_sz + avg_dim;
	window_sz_search0 = window_sz_search / cell_size * cell_size;

	cell_size_search = cell_size;

	//scale_sz = window_sz;
	scale_sz = static_cast<int>(sqrt(target_sz.area())*scale_padding);
	//logPolar featuremap, like window_sz0
	//scale_sz_window = 120;
	scale_sz0 = 120;

	mag = 30;
	train_interp_factor = 0.012;
	interp_factor_scale = 0.015;

	_rotation = 1;
	getTemplates(image);
}

void LDESTracker::getSubWindow(const cv::Mat& image, const char* type) {
	if (strcmp(type, "loc") == 0) {
		if (_rotation) {
			patch = cropImageAffine(image, cur_pos, window_sz, window_sz0, cur_scale, cur_rot_degree);
		}
		else {
			int win = (int)(window_sz*cur_scale);
			patch = cropImage(image, cur_pos, win);
			cv::imshow("patch", patch);
			cv::resize(patch, patch, cv::Size(window_sz0, window_sz0), cv::INTER_LINEAR);
		}
	}
	else if (strcmp(type, "srch") == 0) {
		if (_rotation) {
			patchS = cropImageAffine(image, cur_pos, window_sz_search, window_sz_search0, cur_scale, cur_rot_degree);
		}
		else {
			int win = (int)(window_sz_search*cur_scale);
			patchS = cropImage(image, cur_pos, win);
			cv::resize(patchS, patchS, cv::Size(window_sz_search0, window_sz_search0), cv::INTER_LINEAR);
		}
	}
	else if (strcmp(type, "scale") == 0) {
		if (_rotation) {
			patchL = cropImageAffine(image, cur_pos, scale_sz, scale_sz0, cur_scale, cur_rot_degree);
		}
		else {
			int win = (int)(scale_sz*cur_scale);
			patchL = cropImage(image, cur_pos, win);

			cv::resize(patchL, patchL, cv::Size(scale_sz0, scale_sz0), cv::INTER_LINEAR);
			cv::logPolar(patchL, patchL, cv::Point2f(0.5*patchL.cols, 0.5*patchL.rows), mag, cv::INTER_LINEAR);
			cv::imshow("patchL", patchL);
		}
	}
	else
		assert(0);
}
	
void LDESTracker::getTemplates(const cv::Mat& image) {
	getSubWindow(image, "loc");
	getSubWindow(image, "scale");
	getSubWindow(image, "srch");

	cv::imshow("template", patchL);

	cv::Mat empty_;
	cv::Mat x = getFeatures(patch, hann, size_patch, true);
	cv::Mat xs = getFeatures(patchS, hann_search, size_search, true);
	cv::Mat xl;
	if (!_scale_hog)
		xl = getPixFeatures(patchL, size_scale);
	else
		xl = getFeatures(patchL, empty_, size_scale); 

	createGaussianPeak(size_patch[0], size_patch[1]);

	_alphaf = cv::Mat(size_patch[0], size_patch[1], CV_32FC2, float(0));
	_z = cv::Mat(size_patch[2], size_patch[0] * size_patch[1], CV_32F, float(0));
	modelPatch=cv::Mat(size_scale[2], size_scale[0]*size_scale[1], CV_32F, float(0));
	// train with initial frame
	trainLocation(x, 1.0);
	trainScale(xl, 1.0);
}

void LDESTracker::trainLocation(cv::Mat& x, float train_interp_factor) {
	cv::Mat k = gaussianCorrelation(x, x, size_patch[0], size_patch[1], size_patch[2], sigma);
	cv::Mat alphaf = complexDivision(_yf, (k + lambda));

	_z = (1 - train_interp_factor) * _z + (train_interp_factor)* x;
	_alphaf = (1 - train_interp_factor) * _alphaf + (train_interp_factor)* alphaf;
}

void LDESTracker::trainScale(cv::Mat& x, float interp_factor) {
	modelPatch = (1 - interp_factor)*modelPatch + interp_factor * x;
}

cv::Mat LDESTracker::padImage(const cv::Mat& image, int& x1, int& y1, int& x2, int& y2) {
	cv::Mat padded;

	int im_h = image.rows, im_w = image.cols;
	int left, top, right, bottom;

	left = MAX(0, -x1);
	right = MAX(0, x2 - (im_w - 1));
	top = MAX(0, -y1);
	bottom = MAX(0, y2 - (im_h - 1));

	x1 = left;
	x2 = right;
	y1 = top;
	y2 = bottom;

	cv::copyMakeBorder(image, padded, top, bottom, left, right, cv::BORDER_REPLICATE);

	return padded;
}

cv::Mat LDESTracker::cropImage(const cv::Mat& image, const cv::Point2i& pos, int sz) {
	int x1 = pos.x - sz / 2;
	int y1 = pos.y - sz / 2;
	int x2 = pos.x + sz / 2;
	int y2 = pos.y + sz / 2;
	cv::Mat padded = padImage(image, x1, y1, x2, y2);

	cv::Point2i p(pos.x + x1, pos.y + y1);
	cv::Rect rec(p.x - sz / 2, p.y - sz / 2, sz, sz);

	cv::Mat patch;
	padded(rec).copyTo(patch);

	return patch;
}

cv::Mat LDESTracker::cropImageAffine(const cv::Mat& image, const cv::Point2i& pos, int sz, int resize_sz, float scale, float rot) {
	cv::Mat rot_matrix = cv::getRotationMatrix2D(pos, -rot, scale);
	rot_matrix.convertTo(rot_matrix, CV_32F);
	cv::transpose(rot_matrix, rot_matrix);

	float corners_ptr[12] = {
		pos.x - sz / 2,pos.y - sz / 2,1.0,\
		pos.x - sz / 2,pos.y + sz / 2,1.0,\
		pos.x + sz / 2,pos.y + sz / 2,1.0,\
		pos.x + sz / 2,pos.y - sz / 2,1.0
	};
	cv::Mat corners(4, 3, CV_32F, corners_ptr);

	cv::Mat wcorners = corners * rot_matrix;

	double x1, y1, x2, y2;
	cv::minMaxLoc(wcorners.col(0).clone(), &x1, &x2, NULL, NULL);
	cv::minMaxLoc(wcorners.col(1).clone(), &y1, &y2, NULL, NULL);

	int ix1 = (int)x1, ix2 = (int)x2, iy1 = (int)y1, iy2 = (int)y2;
	cv::Mat padded = padImage(image, ix1, iy1, ix2, iy2);

	cv::Point2i p(pos.x + ix1, pos.y + iy1);

	rot_matrix = cv::getRotationMatrix2D(p, -rot, 1.0 / scale);
	rot_matrix.convertTo(rot_matrix, CV_32F);
	rot_matrix.at<float>(0, 2) += sz * scale * 0.5 - pos.x;
	rot_matrix.at<float>(1, 2) += sz * scale * 0.5 - pos.y;
	//cv::Mat M1 = cv::Mat::zeros(3, 3, CV_32F);
	//M1.at<float>(2, 2) = 1.0;
	//rot_matrix.copyTo(M1(cv::Rect(0, 0, 3, 2)));

	//float shift_ptr[6] = {
	//	1.0,0,sz / 2 - p.x,
	//	0,1.0,sz / 2 - p.y
	//};
	//cv::Mat shift(2, 3, CV_32F, shift_ptr);
	//cv::Mat M = shift * M1;

	cv::Mat patch;
	cv::warpAffine(padded, patch, rot_matrix, cv::Size(sz*scale,sz*scale));
	cv::imshow("rot_patch", patch);
	cv::resize(patch, patch, cv::Size(resize_sz, resize_sz), cv::INTER_LINEAR);
	
	return patch;
}

void LDESTracker::estimateLocation(cv::Mat& z, cv::Mat x)
{
	cv::Mat kf = gaussianCorrelation(x, z, size_patch[0], size_patch[1], size_patch[2], sigma);
	cv::Mat res = fftd(complexMultiplication(_alphaf, kf), true);
	cv::Mat resmap;
	cv::normalize(res, resmap, 0, 1, cv::NORM_MINMAX);
	cv::imshow("res", resmap);

	cv::Point2i pi;
	double pv;
	cv::minMaxLoc(res, NULL, &pv, NULL, &pi);
	float peak_value = (float)pv;

	cscore=calcPSR(res, pi);

	//subpixel peak estimation, coordinates will be non-integer
	cv::Point2f p((float)pi.x, (float)pi.y);

	if (pi.x > 0 && pi.x < res.cols - 1) {
		p.x += subPixelPeak(res.at<float>(pi.y, pi.x - 1), peak_value, res.at<float>(pi.y, pi.x + 1));
	}

	if (pi.y > 0 && pi.y < res.rows - 1) {
		p.y += subPixelPeak(res.at<float>(pi.y - 1, pi.x), peak_value, res.at<float>(pi.y + 1, pi.x));
	}
	//Different from C++ code and MATLAB code
	float hori_delta = p.x, ver_delta = p.y;
	hori_delta -= (res.cols) / 2;
	ver_delta -= (res.rows) / 2;

	cout << hori_delta << ',' << ver_delta << endl;
	if (_rotation) {
		float cs = cos(cur_rot_degree), sn = sin(cur_rot_degree);
		float dx = cell_size * hori_delta*cs + cell_size * ver_delta*sn;
		float dy = -cell_size * hori_delta*sn + cell_size * ver_delta*cs;
		cur_pos.x = MIN(cur_pos.x + dx, im_width - 1);
		cur_pos.y = MIN(cur_pos.y + dy, im_height - 1);
	}
	else {
		cur_pos.x = MIN(cur_pos.x + hori_delta * cell_size*cur_scale, im_width - 1);
		cur_pos.y = MIN(cur_pos.y + ver_delta * cell_size*cur_scale, im_height - 1);
		//cur_roi.x = cur_pos.x - target_sz.width / 2;
		//cur_roi.y = cur_pos.y - target_sz.height / 2;
	}
}

void LDESTracker::estimateScale(cv::Mat& z, cv::Mat& x) {
	cv::Mat rf = phaseCorrelation(x, z, size_scale[0], size_scale[1], size_scale[2]);
	cv::Mat res = fftd(rf, true);
	rearrange(res);
	cv::Mat resmap;
	float _upsample = 2.0;
	cv::resize(res, res, cv::Size(0, 0), _upsample, _upsample, cv::INTER_LINEAR);
	cv::normalize(res, resmap, 0, 1, cv::NORM_MINMAX);
	cv::imshow("phase", resmap);

	size_scale[0] *= _upsample;
	size_scale[1] *= _upsample;
	cv::Rect center(5, 5, size_scale[1] - 10, size_scale[0] - 10);
	res = res(center);

	cv::Point2i pi;
	double pv;
	cv::minMaxLoc(res, NULL, &pv, NULL, &pi);
	pi.x += 5;
	pi.y += 5;
	pi.x -= size_scale[1] * 0.5;
	pi.y -= size_scale[0] * 0.5;

	pi.x /= _upsample;
	pi.y /= _upsample;
	size_scale[0] /= _upsample;
	size_scale[1] /= _upsample;

	float rot = -(pi.y) * 180.0 / (size_scale[0] * 0.5);
	float scale = exp((pi.x) / mag);

	sscore = static_cast<float>(pv);

	delta_rot = rot;
	delta_scale = scale;
}

void LDESTracker::update(cv::Mat image) {
	//update BGD
	im_height = image.rows;
	im_width = image.cols;
	updateModel(image, 0);
	float mcscore = 0, mscale, mrot;
	cv::Point2f mpos;
	for (int i = 0; i < 1; ++i) {
		cscore = (1 - interp_n)*cscore + interp_n * sscore;
		if (window_sz0*cur_scale < 5)
			delta_scale = 1.0;
		cur_scale *= delta_scale;
		cur_rot_degree += delta_rot;
		if (cscore >= mcscore) {
			mscale = cur_scale;
			mrot = cur_rot_degree;
			mpos = cur_pos;
			mcscore = cscore;
		}
		else
			break;
		updateModel(image, i);
	}
	cur_pos = mpos;
	cur_scale = mscale;
	cur_rot_degree = mrot;

	getSubWindow(image, "scale");

	cv::Mat x = getFeatures(patch, hann, size_patch, false);
	//cv::Mat xl = getFeatures(patchL, cv::Mat(), size_scale, false);
	cv::Mat xl = getPixFeatures(patchL, size_scale);

	trainLocation(x, train_interp_factor);
	trainScale(xl, interp_factor_scale);
}

void LDESTracker::updateModel(cv::Mat& image, int polish) {
	int w_sz0;
	cv::Mat _han, empty_;
	im_height = image.rows;
	im_width = image.cols;
	if (polish >= 0) {	
		getSubWindow(image, "loc");

		cv::Mat x = getFeatures(patch, hann, size_patch, false);
		estimateLocation(_z, x);

		getSubWindow(image, "scale");
		cv::Mat xl;
		if(!_scale_hog)
			xl= getPixFeatures(patchL, size_scale);
		else
			xl = getFeatures(patchL, empty_, size_scale);

		estimateScale(modelPatch, xl);
		
		delta_scale = MAX(MIN(delta_scale, 1.2), 0.8);
		delta_rot = MAX(MIN(delta_rot, 6), -6);

		cur_rot_degree += delta_rot;
		cur_scale *= delta_scale;

		cout << "Cur scale: " << cur_scale <<" cur rotation:  "<<cur_rot_degree<<endl;
		cur_roi.width = target_sz0.width*cur_scale;
		cur_roi.height = target_sz0.height*cur_scale;
		cur_roi.x = cur_pos.x - cur_roi.width / 2;
		cur_roi.y = cur_pos.y - cur_roi.height / 2;

		getSubWindow(image, "loc");
		getSubWindow(image, "scale");
		
		x = getFeatures(patch, hann, size_patch, false);
		
		if (!_scale_hog)
			xl = getPixFeatures(patchL, size_scale);
		else
			xl = getFeatures(patchL, empty_, size_scale);
		
		trainLocation(x, train_interp_factor);
		trainScale(xl, interp_factor_scale);
	}
	else {
		w_sz0 = window_sz_search0;
		_han = hann_search;
	}
}

void LDESTracker::createGaussianPeak(int sizey, int sizex) {
	cv::Mat_<float> res(sizey, sizex);

	int syh = (sizey) / 2; 
	int sxh = (sizex) / 2;

	float output_sigma = std::sqrt((float)sizex * sizey) / cell_size * output_sigma_factor;
	float mult = -0.5 / (output_sigma * output_sigma);

	for (int i = 0; i < sizey; i++)
		for (int j = 0; j < sizex; j++)
		{
			int ih = i - syh;
			int jh = j - sxh;
			res(i, j) = std::exp(mult * (float)(ih * ih + jh * jh));
		}

	res.copyTo(_y);
	_yf = fftd(_y);
}

cv::Mat LDESTracker::getFeatures(const cv::Mat & patch, cv::Mat& han, int* sizes, bool inithann)
{
	cv::Mat FeaturesMap;
	// HOG features
	IplImage z_ipl = patch;
	CvLSVMFeatureMapCaskade *map;
	getFeatureMaps(&z_ipl, cell_size, &map);
	normalizeAndTruncate(map, 0.2f);
	PCAFeatureMaps(map);
	sizes[0] = map->sizeY;
	sizes[1] = map->sizeX;
	sizes[2] = map->numFeatures;

	FeaturesMap = cv::Mat(cv::Size(map->numFeatures, map->sizeX*map->sizeY), CV_32F, map->map);  // Procedure do deal with cv::Mat multichannel bug
	FeaturesMap = FeaturesMap.t();
	freeFeatureMapObject(&map);

	if (inithann) {		
		cv::Size hannSize(sizes[1], sizes[0]);
		cv::Mat hannsMat = hann3D(hannSize, sizes[2]);
		hannsMat.copyTo(han);
		FeaturesMap = han.mul(FeaturesMap);
	}
	else if (!han.empty())
		FeaturesMap = han.mul(FeaturesMap);
	//std::cout << "feature map size: " << size_patch[0] << ',' << size_patch[1] << std::endl;
	return FeaturesMap;
}

cv::Mat LDESTracker::getPixFeatures(const cv::Mat& patch, int* size) {
	int h = patch.rows, w = patch.cols;
	cv::Mat features(patch.channels(), w*h, CV_32F);
	vector<cv::Mat > planes(3);
	cv::split(patch, planes);
	planes[0].reshape(1, 1).copyTo(features.row(0));
	planes[1].reshape(1, 1).copyTo(features.row(1));
	planes[2].reshape(1, 1).copyTo(features.row(2));
	size[0] = h;
	size[1] = w;
	size[2] = patch.channels();
	return features;
}

float LDESTracker::subPixelPeak(float left, float center, float right) {
	float divisor = 2 * center - right - left;
	if (divisor == 0)
		return 0;
	return 0.5 * (right - left) / divisor;
}

float LDESTracker::calcPSR(const cv::Mat& res, cv::Point2i& peak_loc) {
	return res.at<float>(peak_loc.y, peak_loc.x);
}

cv::Rect LDESTracker::testKCFTracker(const cv::Mat& image, cv::Rect& rect, bool init) {
	im_width = image.cols;
	im_height = image.rows;
	if (init) {
		_rotation = false;
		this->init(rect, image);		
		return cv::Rect();
	}
	else {
		getSubWindow(image, "loc");
		cv::Mat win_img = image.clone();
		cv::Rect rec(cur_pos.x - window_sz / 2, cur_pos.y - window_sz / 2, window_sz, window_sz);
		cv::rectangle(win_img, rec, cv::Scalar(0, 255, 0), 2);
		
		cv::Mat x = getFeatures(patch, hann, size_patch, false);
		estimateLocation(_z, x);
		x = getFeatures(patch, hann, size_patch, false);
		cv::circle(win_img, cur_pos, 4, cv::Scalar(0, 0, 255), -1);
		cv::imshow("window", win_img);
		cur_roi.width =target_sz0.width*cur_scale;
		cur_roi.height = target_sz0.height*cur_scale;
		cur_roi.x = cur_pos.x - cur_roi.width / 2;
		cur_roi.y = cur_pos.y - cur_roi.height / 2;
		trainLocation(x, train_interp_factor);
		return cur_roi;
	}
}